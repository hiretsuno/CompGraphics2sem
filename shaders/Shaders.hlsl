
cbuffer PassCB : register(b0)
{
    float4x4 gWorld;        // Object-to-world
    float4x4 gViewProj;     // View x Projection
    float4x4 gInvViewProj;  // Inverse of ViewProj
    float4   gEyePosW;      // xyz = eye world position
    float4   gRTSize;       // x=width, y=height, z=1/width, w=1/height
    float4   gTessParams;   // x = near dist (max tess), y = far dist (min tess), z = min factor, w = max factor
    float4   gDispParams;   // x = displacement scale (world units), y = height value that means "no displacement"
};

cbuffer MaterialCB : register(b2)
{
    float4 gBaseColor;      // rgb = diffuse tint,  a unused
    float4 gSurfaceParams;  // x = specular intensity, y = shininess
};

// Material textures (3 consecutive descriptors per draw call).
// t1..t3 are the G-buffer targets used by the lighting pass, so the extra maps start at t4.
Texture2D    gDiffuseMap : register(t0);
Texture2D    gNormalMap  : register(t4);   // tangent-space normal map (DirectX convention, green = down)
Texture2D    gDispMap    : register(t5);   // height map, sampled in the Domain Shader
SamplerState gSampler : register(s0);

struct VSIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

// Vertex Shader -> Hull Shader. Everything is kept in world space: the vertex only becomes
// a clip-space position in the Domain Shader, AFTER it has been displaced.
struct VSOut
{
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
};

// Domain Shader -> Pixel Shader
struct GeoVSOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
};

VSOut GeometryVS(VSIn vin)
{
    VSOut vout;
    vout.PosW = mul(float4(vin.PosL, 1.f), gWorld).xyz;
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorld);
    vout.TexC = vin.TexC;
    return vout;
}

// ---------------------------------------------------------------------------
// Hull Shader: decides HOW MUCH to subdivide each triangle patch
// ---------------------------------------------------------------------------

// Tessellation factor from the distance to the camera:
// distance <= near -> max factor, distance >= far -> min factor, linear in between.
float CalcTessFactor(float3 posW)
{
    float d = distance(posW, gEyePosW.xyz);
    float s = saturate((gTessParams.y - d) / max(gTessParams.y - gTessParams.x, 1e-4f));
    return lerp(gTessParams.z, gTessParams.w, s);
}

struct PatchTess
{
    float EdgeTess[3] : SV_TessFactor;
    float InsideTess : SV_InsideTessFactor;
};

// Runs once per patch (triangle).
PatchTess PatchConstantHS(InputPatch<VSOut, 3> patch)
{
    PatchTess pt;

    // Each EDGE factor is computed from that edge's midpoint only, so two neighbouring
    // triangles that share an edge always get the same factor -> no cracks between patches.
    // For the "tri" domain edge i is the edge opposite to control point i.
    pt.EdgeTess[0] = CalcTessFactor(0.5f * (patch[1].PosW + patch[2].PosW));
    pt.EdgeTess[1] = CalcTessFactor(0.5f * (patch[2].PosW + patch[0].PosW));
    pt.EdgeTess[2] = CalcTessFactor(0.5f * (patch[0].PosW + patch[1].PosW));

    pt.InsideTess = (pt.EdgeTess[0] + pt.EdgeTess[1] + pt.EdgeTess[2]) / 3.f;
    return pt;
}

// Runs once per control point (3 times per patch); here it just passes the data through.
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchConstantHS")]
[maxtessfactor(64.0f)]
VSOut GeometryHS(InputPatch<VSOut, 3> patch, uint i : SV_OutputControlPointID)
{
    return patch[i];
}

// ---------------------------------------------------------------------------
// Domain Shader: runs for every vertex produced by the tessellator
// ---------------------------------------------------------------------------
[domain("tri")]
GeoVSOut GeometryDS(PatchTess patchTess, float3 bary : SV_DomainLocation, const OutputPatch<VSOut, 3> tri)
{
    GeoVSOut dout;

    // bary = barycentric coordinates of the new vertex inside the original triangle
    float3 posW = bary.x * tri[0].PosW + bary.y * tri[1].PosW + bary.z * tri[2].PosW;
    float3 normalW = normalize(bary.x * tri[0].NormalW + bary.y * tri[1].NormalW + bary.z * tri[2].NormalW);
    float2 uv = bary.x * tri[0].TexC + bary.y * tri[1].TexC + bary.z * tri[2].TexC;

    // The Domain Shader has no screen-space derivatives, so Sample() is not allowed here:
    // the mip level is given explicitly with SampleLevel().
    float height = gDispMap.SampleLevel(gSampler, uv, 0.f).r;
    posW += normalW * ((height - gDispParams.y) * gDispParams.x);

    dout.PosW = posW;
    dout.NormalW = normalW;
    dout.TexC = uv;
    dout.PosH = mul(float4(posW, 1.f), gViewProj);
    return dout;
}

struct GBufferOut
{
    float4 AlbedoSpec : SV_Target0;
    float4 Normal : SV_Target1;
    float  Depth : SV_Target2;
};

// Builds a tangent frame from screen-space derivatives (the mesh has no tangents),
// and turns the tangent-space normal from the map into a world-space normal.
float3 ApplyNormalMap(float3 N, float3 posW, float2 uv)
{
    // [0,1] -> [-1,1]
    float3 nTS = gNormalMap.Sample(gSampler, uv).xyz * 2.f - 1.f;

    float3 dp1 = ddx(posW);
    float3 dp2 = ddy(posW);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);

    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    float invMax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20f));
    float3x3 TBN = float3x3(T * invMax, B * invMax, N); // rows = T, B, N

    return normalize(mul(nTS, TBN));
}

GBufferOut GeometryPS(GeoVSOut pin)
{
    GBufferOut gout;

    float3 albedo = gDiffuseMap.Sample(gSampler, pin.TexC).rgb * gBaseColor.rgb;
    float  specInt = gSurfaceParams.x;
    float  shiny = gSurfaceParams.y;

    float3 N = ApplyNormalMap(normalize(pin.NormalW), pin.PosW, pin.TexC);

    gout.AlbedoSpec = float4(albedo, specInt);
    gout.Normal = float4(N, shiny);
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
