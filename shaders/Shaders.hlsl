cbuffer PassCB : register(b0)
{
    float4x4 gWorld;        // Object-to-world
    float4x4 gViewProj;     // View × Projection
    float4x4 gInvViewProj;  // Inverse of ViewProj
    float4   gEyePosW;      // xyz = eye world position
    float4   gRTSize;       // x=width, y=height, z=1/width, w=1/height
};

cbuffer MaterialCB : register(b2)
{
    float4 gBaseColor;      // rgb = diffuse tint,  a unused
    float4 gSurfaceParams;  // x = specular intensity, y = shininess
};

Texture2D    gDiffuseMap : register(t0);
SamplerState gSampler : register(s0);

struct VSIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

struct GeoVSOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
};

GeoVSOut GeometryVS(VSIn vin)
{
    GeoVSOut vout;
    float4 posW = mul(float4(vin.PosL, 1.f), gWorld);
    vout.PosW = posW.xyz;
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorld);
    vout.PosH = mul(posW, gViewProj);
    vout.TexC = vin.TexC;
    return vout;
}

// ---------------- Shadow pass: только глубина в текущий каскад ----------------
cbuffer CascadeCB : register(b4)
{
    float4x4 gCascadeViewProj;   // LightViewProj текущего каскада (root constants)
};

float4 ShadowVS(VSIn vin) : SV_POSITION
{
    float4 posW = mul(float4(vin.PosL, 1.f), gWorld);
    return mul(posW, gCascadeViewProj);
}

struct GBufferOut
{
    float4 AlbedoSpec : SV_Target0;
    float4 Normal : SV_Target1;
    float  Depth : SV_Target2;
};

GBufferOut GeometryPS(GeoVSOut pin)
{
    GBufferOut gout;

    float3 albedo = gDiffuseMap.Sample(gSampler, pin.TexC).rgb * gBaseColor.rgb;
    float  specInt = gSurfaceParams.x;
    float  shiny = gSurfaceParams.y;

    gout.AlbedoSpec = float4(albedo, specInt);
    gout.Normal = float4(normalize(pin.NormalW), shiny);
    gout.Depth = pin.PosH.z;
    return gout;
}


struct GpuLight
{
    float4 PositionRange;   // xyz = position,  w = range
    float4 DirectionSpot;   // xyz = direction, w = cos(outerAngle)
    float4 ColorIntensity;  // rgb = color,     a = intensity
    float4 Params;          // x = type (0=dir, 1=point, 2=spot), y = cos(innerAngle), z = casts shadow (только dir)
};

#define MAX_LIGHTS 32

cbuffer LightCB : register(b1)
{
    float4   gAmbientColor;         // rgb = ambient, a unused
    float4   gLightCount;           // x = number of active lights
    GpuLight gLights[MAX_LIGHTS];
};

Texture2D gAlbedoSpecTex : register(t1);
Texture2D gNormalTex : register(t2);
Texture2D gDepthTex : register(t3);

// ---------------- CSM ----------------
#define CASCADE_COUNT 4
#define SHADOW_MAP_SIZE 2048.0

cbuffer ShadowCB : register(b3)
{
    float4x4 gLightViewProj[CASCADE_COUNT];  // World -> clip света для каждого каскада
    float4   gCascadeSplits;                 // дальние границы каскадов (расстояние вдоль взгляда)
    float4   gTexelWorld;                    // размер текселя каскада в метрах
};

Texture2DArray<float>  gShadowMap     : register(t4);
SamplerComparisonState gShadowSampler : register(s1);   // аппаратное сравнение + билинейный PCF

// Возвращает 1 = свет, 0 = тень. viewZ — глубина пикселя вдоль взгляда камеры.
float ShadowFactor(float3 posW, float3 N, float3 L, float viewZ)
{
    // 1. Выбор каскада по глубине
    int c = 0;
    if (viewZ > gCascadeSplits.x) c = 1;
    if (viewZ > gCascadeSplits.y) c = 2;
    if (viewZ > gCascadeSplits.z) c = 3;
    if (viewZ > gCascadeSplits.w) return 1.f;   // дальше дальности теней

    // 2. Bias с учётом наклона поверхности (всё в метрах, кратно размеру текселя каскада)
    float texelWorld = gTexelWorld[c];
    float NdotL      = saturate(dot(N, L));
    float tanTheta   = min(sqrt(1.f - NdotL * NdotL) / max(NdotL, 0.05f), 3.f);

    float normalOffset = texelWorld * 1.5f;                     // сдвиг вдоль нормали убирает acne на скосах
    float slopeBias    = texelWorld * (1.f + 1.5f * tanTheta);  // сдвиг к свету растёт с наклоном
    float3 biasedPos   = posW + N * normalOffset + L * slopeBias;

    // 3. В пространство света -> UV карты и эталонная глубина
    float4 lightPos = mul(float4(biasedPos, 1.f), gLightViewProj[c]);
    float2 uv       = lightPos.xy * float2(0.5f, -0.5f) + 0.5f;
    float  depth    = lightPos.z;

    // 4. PCF 3x3: 9 выборок, каждая ещё и билинейно фильтруется железом
    const float texelUV = 1.f / SHADOW_MAP_SIZE;
    float lit = 0.f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * texelUV;
            lit += gShadowMap.SampleCmpLevelZero(gShadowSampler, float3(uv + offset, c), depth);
        }
    }
    return lit / 9.f;
}

struct QuadVSOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

QuadVSOut LightingVS(uint id : SV_VertexID)
{
    QuadVSOut vout;
    // Generate clip-space triangle covering the whole screen
    vout.TexC = float2((id << 1) & 2, id & 2);
    vout.PosH = float4(vout.TexC * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);
    return vout;
}

// Reconstruct world-space position from NDC depth + pixel UV
float3 ReconstructWorldPos(float2 uv, float ndcDepth, out float viewZ)
{
    // uv in [0,1], convert to NDC xy
    float4 clipPos = float4(uv * float2(2.f, -2.f) + float2(-1.f, 1.f),
                            ndcDepth, 1.f);
    float4 worldPos = mul(clipPos, gInvViewProj);
    viewZ = 1.f / worldPos.w;   // worldPos.w = 1 / clip.w, а clip.w = глубина вдоль взгляда
    return worldPos.xyz / worldPos.w;
}

float4 LightingPS(QuadVSOut pin) : SV_TARGET
{
    int3 coords = int3((int2)pin.PosH.xy, 0);

    float4 albedoSpec = gAlbedoSpecTex.Load(coords);
    // Текстуры хранятся в sRGB: считаем освещение в линейном пространстве, гамма — на выходе.
    float3 albedo = pow(albedoSpec.rgb, 2.2f);
    float  specInt = albedoSpec.a;

    float4 normalSample = gNormalTex.Load(coords);
    float3 N = normalize(normalSample.xyz);
    float  shininess = normalSample.a;
    shininess = max(shininess, 1.f);  // avoid pow(x,0)

    float  ndcDepth = gDepthTex.Load(coords).r;

    // Skip background pixels (depth == 1 means nothing was written)
    if (ndcDepth >= 1.f)
        return float4(0.f, 0.f, 0.f, 1.f);

    float2 uv = pin.PosH.xy * gRTSize.zw;
    float  viewZ;
    float3 posW = ReconstructWorldPos(uv, ndcDepth, viewZ);
    float3 V = normalize(gEyePosW.xyz - posW);

    // Ambient
    float3 finalColor = gAmbientColor.rgb * albedo;

    int lightCount = (int)gLightCount.x;
    for (int i = 0; i < lightCount; ++i)
    {
        GpuLight light = gLights[i];
        float  type = light.Params.x;
        float3 lightColor = light.ColorIntensity.rgb;
        float  intensity = light.ColorIntensity.a;
        float3 L;
        float  attenuation = 1.f;

        if (type < 0.5f)
        {
            // ---- Directional ----
            L = normalize(-light.DirectionSpot.xyz);
            if (light.Params.z > 0.5f)   // Params.z = 1: источник отбрасывает каскадные тени
                attenuation = ShadowFactor(posW, N, L, viewZ);
        }
        else if (type < 1.5f)
        {
            // ---- Point ----
            float3 toLight = light.PositionRange.xyz - posW;
            float  dist = length(toLight);
            float  range = light.PositionRange.w;
            if (dist >= range) continue;
            L = toLight / dist;
            float t = dist / range;
            attenuation = saturate(1.f - t * t);   // smooth inverse-square falloff
        }
        else
        {
            // ---- Spot ----
            float3 toLight = light.PositionRange.xyz - posW;
            float  dist = length(toLight);
            float  range = light.PositionRange.w;
            if (dist >= range) continue;
            L = toLight / dist;

            float  cosOuter = light.DirectionSpot.w;
            float  cosInner = light.Params.y;
            float  cosAngle = dot(-L, normalize(light.DirectionSpot.xyz));
            if (cosAngle <= cosOuter) continue;

            float  denom = max(cosInner - cosOuter, 1e-4f);
            float  spotFactor = saturate((cosAngle - cosOuter) / denom);
            float  t = dist / range;
            attenuation = saturate(1.f - t * t) * spotFactor;
        }

        float  NdotL = max(dot(N, L), 0.f);
        float3 H = normalize(L + V);
        float  NdotH = max(dot(N, H), 0.f);
        float  spec = specInt * pow(NdotH, shininess);

        finalColor += (albedo * NdotL + spec) * lightColor * intensity * attenuation;
    }

    return float4(pow(saturate(finalColor), 1.f / 2.2f), 1.f);
}