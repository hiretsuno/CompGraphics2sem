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
    float4 gBaseColor;      // rgb = albedo tint,  a unused
    float4 gSurfaceParams;  // x = metallic factor, y = roughness factor, z = ao (множители на gMetalRoughMap.b/.g)
};

Texture2D    gDiffuseMap    : register(t0);    // albedo (sRGB-данные, декодируются в lighting pass)
Texture2D    gNormalMap     : register(t9);    // тангенциальная normal map; 1x1 заглушка = (0.5, 0.5, 1)
Texture2D    gMetalRoughMap : register(t10);   // map_MR: r = metallic, g = roughness (проверено по пикселям Cerberus_MR.jpg — не совпадает с glTF g/b!); 1x1 заглушка = (1, 1, _)
SamplerState gSampler : register(s0);

// Если у normal map зелёный канал "вверх" по OpenGL, а не по DirectX — поставить 1.
#define NORMAL_MAP_FLIP_Y 0

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
    float4 Material : SV_Target3;   // r = metallic, g = roughness, b = ao
};

// Базис TBN из производных позиции и UV (без тангентов в вершинах). Строки матрицы: T, B, N.
float3x3 CotangentFrame(float3 N, float3 posW, float2 uv)
{
    float3 dp1 = ddx(posW);
    float3 dp2 = ddy(posW);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);

    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    float invMax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20f));
    return float3x3(T * invMax, B * invMax, N);
}

GBufferOut GeometryPS(GeoVSOut pin)
{
    GBufferOut gout;

    float3 albedo = gDiffuseMap.Sample(gSampler, pin.TexC).rgb * gBaseColor.rgb;

    // Normal mapping: тангенциальная нормаль -> мир
    float3 N = normalize(pin.NormalW);
    float3 tn = gNormalMap.Sample(gSampler, pin.TexC).xyz * 2.f - 1.f;
#if NORMAL_MAP_FLIP_Y
    tn.y = -tn.y;
#endif
    N = normalize(mul(normalize(tn), CotangentFrame(N, pin.PosW, pin.TexC)));

    float3 mr = gMetalRoughMap.Sample(gSampler, pin.TexC).rgb;   // заглушка (1,1,_) делает factor'ы обычными константами
    float  metallic  = saturate(gSurfaceParams.x * mr.r);
    float  roughness = saturate(gSurfaceParams.y * mr.g);

    gout.AlbedoSpec = float4(albedo, 1.f);
    gout.Normal = float4(N, 0.f);
    gout.Depth = pin.PosH.z;
    gout.Material = float4(metallic, roughness, gSurfaceParams.z, 1.f);
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
    float4   gPbrDebug;             // x = подмена материала (0..3), y = IBL-ambient вкл, z = прямой свет вкл
    float4   gLightCount;           // x = number of active lights
    GpuLight gLights[MAX_LIGHTS];
};

Texture2D gAlbedoSpecTex : register(t1);   // rgb = albedo (sRGB)
Texture2D gNormalTex : register(t2);       // xyz = world normal
Texture2D gDepthTex : register(t3);        // R32_FLOAT, NDC depth
Texture2D gMaterialTex : register(t4);     // r = metallic, g = roughness, b = ao

// ---------------- CSM ----------------
#define CASCADE_COUNT 4
#define SHADOW_MAP_SIZE 2048.0

cbuffer ShadowCB : register(b3)
{
    float4x4 gLightViewProj[CASCADE_COUNT];  // World -> clip света для каждого каскада
    float4   gCascadeSplits;                 // дальние границы каскадов (расстояние вдоль взгляда)
    float4   gTexelWorld;                    // размер текселя каскада в метрах
};

Texture2DArray<float>  gShadowMap     : register(t5);
SamplerComparisonState gShadowSampler : register(s1);   // аппаратное сравнение + билинейный PCF

// ---------------- IBL (split-sum) ----------------
// Слоты 6..8 SRV-кучи GBuffer, см. iblRange в BuildRootSignature.
TextureCube irradianceMap : register(t6);   // диффузная часть, сэмплируется по N
TextureCube prefilterMap  : register(t7);   // зеркальная часть, mip = roughness * MAX_REFLECTION_LOD
Texture2D   brdfLUT       : register(t8);   // (NdotV, roughness) -> (scale, bias) для F0

SamplerState irradianceMapSampler : register(s2);   // trilinear clamp, общий для обоих кубмапов
SamplerState brdfLUTSampler       : register(s3);   // bilinear clamp
#define prefilterMapSampler irradianceMapSampler    // те же настройки -> один статический сэмплер

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

// ---------------- PBR: Cook-Torrance BRDF ----------------
static const float PI = 3.14159265359f;

// D: Trowbridge-Reitz GGX. a = roughness^2
float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a      = roughness * roughness;
    float a2     = a * a;
    float NdotH  = max(dot(N, H), 0.f);
    float NdotH2 = NdotH * NdotH;

    float denom = NdotH2 * (a2 - 1.f) + 1.f;
    return a2 / (PI * denom * denom);
}

// G_sub: Schlick-GGX
float GeometrySchlickGGX(float NdotV, float k)
{
    return NdotV / (NdotV * (1.f - k) + k);
}

// G: метод Смита, k для аналитических источников света = (roughness + 1)^2 / 8
float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float k = (roughness + 1.f) * (roughness + 1.f) / 8.f;
    float NdotV = max(dot(N, V), 0.f);
    float NdotL = max(dot(N, L), 0.f);
    return GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);
}

// F: Fresnel-Schlick
float3 fresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.f - F0) * pow(clamp(1.f - cosTheta, 0.f, 1.f), 5.f);
}

// F для ambient: с учётом roughness (иначе шероховатые поверхности дают слишком яркий ободок)
float3 fresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(float3(1.f - roughness, 1.f - roughness, 1.f - roughness), F0) - F0) * pow(clamp(1.f - cosTheta, 0.f, 1.f), 5.f);
}

float4 LightingPS(QuadVSOut pin) : SV_TARGET
{
    int3 coords = int3((int2)pin.PosH.xy, 0);

    float4 albedoSpec = gAlbedoSpecTex.Load(coords);
    // Текстуры хранятся в sRGB: считаем освещение в линейном пространстве, гамма — на выходе.
    float3 albedo = pow(albedoSpec.rgb, 2.2f);

    float3 N = normalize(gNormalTex.Load(coords).xyz);

    float3 material  = gMaterialTex.Load(coords).rgb;
    float  metallic  = material.r;
    float  roughness = material.g;
    float  ao        = material.b;

    // Отладочная подмена материала (F4): чтобы увидеть металл / глянец на сцене без металлических текстур
    if (gPbrDebug.x > 2.5f)      { metallic = 0.f; roughness = 0.1f; }   // 3: глянцевый диэлектрик
    else if (gPbrDebug.x > 1.5f) { metallic = 0.f; roughness = 1.f; }    // 2: шероховатый диэлектрик
    else if (gPbrDebug.x > 0.5f) { metallic = 1.f; roughness = 0.15f; }  // 1: хром
    roughness = clamp(roughness, 0.05f, 1.f);   // при 0 D и G вырождаются

    float  ndcDepth = gDepthTex.Load(coords).r;

    // Skip background pixels (depth == 1 means nothing was written)
    if (ndcDepth >= 1.f)
        return float4(0.f, 0.f, 0.f, 1.f);

    float2 uv = pin.PosH.xy * gRTSize.zw;
    float  viewZ;
    float3 posW = ReconstructWorldPos(uv, ndcDepth, viewZ);
    float3 V = normalize(gEyePosW.xyz - posW);

    // Металлический workflow: диэлектрик F0 = 0.04, металл — F0 = albedo
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    float3 Lo = float3(0.f, 0.f, 0.f);   // прямой свет

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

        float3 radiance = lightColor * intensity * attenuation;

        // Cook-Torrance: fr = kd * albedo / PI + ks * (D * F * G) / (4 * (wo.n) * (wi.n))
        float3 H = normalize(V + L);
        float  NdotL = max(dot(N, L), 0.f);
        float  NdotV = max(dot(N, V), 0.f);

        float  D = DistributionGGX(N, H, roughness);
        float  G = GeometrySmith(N, V, L, roughness);
        float3 F = fresnelSchlick(max(dot(H, V), 0.f), F0);

        float3 specular = (D * G * F) / (4.f * NdotV * NdotL + 0.0001f);   // ks уже внутри F

        float3 kS = F;
        float3 kD = (1.f - kS) * (1.f - metallic);   // металл не имеет диффузной части

        Lo += (kD * albedo / PI + specular) * radiance * NdotL;
    }

    // ---------------- Ambient: IBL (split-sum, слайд 79) ----------------
    float3 normal  = N;
    float3 viewDir = V;
    float3 R = reflect(-viewDir, normal);

    float3 F  = fresnelSchlickRoughness(max(dot(normal, viewDir), 0.f), F0, roughness);
    float3 kS = F;
    float3 kD = (1.f - kS) * (1.f - metallic);

    float3 irradiance = irradianceMap.Sample(irradianceMapSampler, normal).rgb;
    float3 diffuse    = irradiance * albedo;

    uint envW, envH, envLevels;
    prefilterMap.GetDimensions(0, envW, envH, envLevels);
    float MAX_REFLECTION_LOD = (float)(envLevels - 1);

    float3 prefilteredColor = prefilterMap.SampleLevel(prefilterMapSampler, R, roughness * MAX_REFLECTION_LOD).rgb;
    float2 brdf     = brdfLUT.Sample(brdfLUTSampler, float2(max(dot(normal, viewDir), 0.f), roughness)).rg;
    float3 specular = prefilteredColor * (F * brdf.x + brdf.y);

    float3 ambient = (kD * diffuse + specular) * ao;

    float3 finalColor = Lo * gPbrDebug.z + ambient * gPbrDebug.y;

    // HDR -> LDR (Reinhard), затем гамма
    finalColor = finalColor / (finalColor + 1.f);
    return float4(pow(saturate(finalColor), 1.f / 2.2f), 1.f);
}