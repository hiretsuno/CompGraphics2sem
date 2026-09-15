
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
    float4 Params;          // x = type (0=dir, 1=point, 2=spot), y = cos(innerAngle)
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
float3 ReconstructWorldPos(float2 uv, float ndcDepth)
{
    // uv in [0,1], convert to NDC xy
    float4 clipPos = float4(uv * float2(2.f, -2.f) + float2(-1.f, 1.f),
                            ndcDepth, 1.f);
    float4 worldPos = mul(clipPos, gInvViewProj);
    return worldPos.xyz / worldPos.w;
}

float4 LightingPS(QuadVSOut pin) : SV_TARGET
{
    int3 coords = int3((int2)pin.PosH.xy, 0);

    float4 albedoSpec = gAlbedoSpecTex.Load(coords);
    float3 albedo = albedoSpec.rgb;
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
    float3 posW = ReconstructWorldPos(uv, ndcDepth);
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

    return float4(finalColor, 1.f);
}
