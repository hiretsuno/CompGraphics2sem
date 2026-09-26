cbuffer PassCB : register(b0)
{
    float4x4 gWorld;        // Object-to-world
    float4x4 gViewProj;     // View × Projection
    float4x4 gInvViewProj;  // Inverse of ViewProj
    float4   gEyePosW;      // xyz = eye world position
    float4   gRTSize;       // x=width, y=height, z=1/width, w=1/height
};

// Root constants для lighting-прохода (меняются между draw call-ами)
cbuffer LightPassCB : register(b1)
{
    float4 gAmbientColor;   // rgb = ambient
    uint   gLightOffset;    // индекс первого источника в gLights для текущего draw call
    uint3  gLightPassPad;
};

// Root constants для geometry-прохода (материал текущего draw item)
cbuffer MaterialCB : register(b2)
{
    float4 gBaseColor;      // rgb = diffuse tint,  a unused
    float4 gSurfaceParams;  // x = specular intensity, y = shininess
};

Texture2D    gDiffuseMap : register(t0);
SamplerState gSampler    : register(s0);

// G-buffer (читается в lighting-проходе)
Texture2D gAlbedoSpecTex : register(t1);
Texture2D gNormalTex     : register(t2);
Texture2D gDepthTex      : register(t3);   // SRV на тот же depth buffer, что писался в geometry pass

#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT       1
#define LIGHT_SPOT        2

struct GpuLight
{
    float4 PositionRange;   // xyz = position,  w = range
    float4 DirectionSpot;   // xyz = direction, w = cos(outerAngle)
    float4 ColorIntensity;  // rgb = color,     a = intensity
    float4 Params;          // x = type (0=dir, 1=point, 2=spot), y = cos(innerAngle)
};

// Все источники лежат в одном structured buffer, отсортированные по типу:
// [directional...][point...][spot...]. gLightOffset указывает начало нужной группы.
StructuredBuffer<GpuLight> gLights : register(t4);

//geometry pass
struct VSIn
{
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC    : TEXCOORD;
};

struct GeoVSOut
{
    float4 PosH    : SV_POSITION;
    float3 NormalW : NORMAL;
    float2 TexC    : TEXCOORD;
};

GeoVSOut GeometryVS(VSIn vin)
{
    GeoVSOut vout;
    float4 posW  = mul(float4(vin.PosL, 1.f), gWorld);
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorld);
    vout.PosH    = mul(posW, gViewProj);
    vout.TexC    = vin.TexC;
    return vout;
}

struct GBufferOut
{
    float4 AlbedoSpec : SV_Target0;
    float4 Normal     : SV_Target1;
};

GBufferOut GeometryPS(GeoVSOut pin)
{
    GBufferOut gout;

    float3 albedo = gDiffuseMap.Sample(gSampler, pin.TexC).rgb * gBaseColor.rgb;

    // Тут нет ни одного источника света — только «что это за поверхность»
    gout.AlbedoSpec = float4(albedo, gSurfaceParams.x);
    gout.Normal     = float4(normalize(pin.NormalW), gSurfaceParams.y);
    return gout;
}

// LIGHTING PASS
struct Surface
{
    float3 PosW;
    float3 N;
    float3 Albedo;
    float  SpecInt;
    float  Shininess;
};

// Позиция в мире из глубины: пиксель -> NDC (x, y, depth) -> InvViewProj -> деление на w
float3 ReconstructWorldPos(float2 pixel, float ndcDepth)
{
    float2 uv = pixel * gRTSize.zw;
    float4 clipPos = float4(uv * float2(2.f, -2.f) + float2(-1.f, 1.f), ndcDepth, 1.f);
    float4 worldPos = mul(clipPos, gInvViewProj);
    return worldPos.xyz / worldPos.w;
}

// Читает G-buffer в текущем пикселе. false — фон (геометрии нет).
bool LoadSurface(float4 svPosition, out Surface s)
{
    int3 coords = int3((int2)svPosition.xy, 0);

    float depth = gDepthTex.Load(coords).r;

    float4 albedoSpec = gAlbedoSpecTex.Load(coords);
    float4 normalShin = gNormalTex.Load(coords);

    s.Albedo    = albedoSpec.rgb;
    s.SpecInt   = albedoSpec.a;
    s.N         = normalize(normalShin.xyz);
    s.Shininess = max(normalShin.w, 1.f);
    s.PosW      = ReconstructWorldPos(svPosition.xy, depth);

    return depth < 1.f;
}

// Blinn-Phong для одного источника
float3 EvaluateLight(GpuLight light, Surface s)
{
    float3 L;
    float  attenuation = 1.f;
    int    type = (int)light.Params.x;

    if (type == LIGHT_DIRECTIONAL)
    {
        L = normalize(-light.DirectionSpot.xyz);
    }
    else
    {
        float3 toLight = light.PositionRange.xyz - s.PosW;
        float  dist    = length(toLight);
        float  range   = light.PositionRange.w;
        if (dist >= range)
            return 0.f;

        L = toLight / dist;
        float t = dist / range;
        attenuation = saturate(1.f - t * t);
        attenuation *= attenuation;              // плавный спад к краю объёма

        if (type == LIGHT_SPOT)
        {
            float cosOuter = light.DirectionSpot.w;
            float cosInner = light.Params.y;
            float cosAngle = dot(-L, normalize(light.DirectionSpot.xyz));
            attenuation *= smoothstep(cosOuter, max(cosInner, cosOuter + 1e-4f), cosAngle);
        }
    }

    float3 V = normalize(gEyePosW.xyz - s.PosW);
    float3 H = normalize(L + V);
    float  NdotL = saturate(dot(s.N, L));
    float  spec  = s.SpecInt * pow(saturate(dot(s.N, H)), s.Shininess) * (NdotL > 0.f);

    return (s.Albedo * NdotL + spec) * light.ColorIntensity.rgb * light.ColorIntensity.a * attenuation;
}

//  Fullscreen: ambient + directional
struct FullscreenVSOut
{
    float4 PosH : SV_POSITION;
    nointerpolation uint LightIndex : LIGHTINDEX;
};

// Один треугольник, перекрывающий весь экран (без вершинного буфера)
FullscreenVSOut FullscreenVS(uint id : SV_VertexID, uint instance : SV_InstanceID)
{
    FullscreenVSOut vout;
    float2 uv = float2((id << 1) & 2, id & 2);
    vout.PosH = float4(uv * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);
    vout.LightIndex = gLightOffset + instance;
    return vout;
}

float4 AmbientPS(FullscreenVSOut pin) : SV_TARGET
{
    Surface s;
    if (!LoadSurface(pin.PosH, s))
        return float4(0.f, 0.f, 0.f, 1.f);
    return float4(gAmbientColor.rgb * s.Albedo, 1.f);
}

float4 DirectionalPS(FullscreenVSOut pin) : SV_TARGET
{
    Surface s;
    if (!LoadSurface(pin.PosH, s))
        discard;
    return float4(EvaluateLight(gLights[pin.LightIndex], s), 1.f);
}

//  Light volumes: point (сфера) и spot (конус)
//
//  Меш объёма в локальных координатах:
//    сфера — единичная, центр в 0
//    конус — вершина в 0, ось +Z, длина 1, радиус основания 1
//  VS растягивает его по параметрам источника (range, угол, направление).
//
//  Рисуются ЗАДНИЕ грани с тестом глубины GREATER_EQUAL: пиксель проходит, только если
//  поверхность сцены лежит перед задней стенкой объёма. Так свет не считается для
//  пикселей «за» объёмом, и всё работает, даже когда камера внутри объёма.

struct VolumeVSOut
{
    float4 PosH : SV_POSITION;
    nointerpolation uint LightIndex : LIGHTINDEX;
};

VolumeVSOut LightVolumeVS(float3 PosL : POSITION, uint instance : SV_InstanceID)
{
    VolumeVSOut vout;
    vout.LightIndex = gLightOffset + instance;

    GpuLight light = gLights[vout.LightIndex];
    float3 center = light.PositionRange.xyz;
    float  range  = light.PositionRange.w;

    float3 posW;
    if ((int)light.Params.x == LIGHT_SPOT)
    {
        float3 fwd   = normalize(light.DirectionSpot.xyz);
        float3 up    = abs(fwd.y) < 0.99f ? float3(0.f, 1.f, 0.f) : float3(1.f, 0.f, 0.f);
        float3 right = normalize(cross(up, fwd));
        up = cross(fwd, right);

        float cosOuter = light.DirectionSpot.w;
        float radius   = range * sqrt(saturate(1.f - cosOuter * cosOuter)) / cosOuter; // range * tan(outer)

        posW = center + right * (PosL.x * radius) + up * (PosL.y * radius) + fwd * (PosL.z * range);
    }
    else
    {
        posW = center + PosL * range;
    }

    vout.PosH = mul(float4(posW, 1.f), gViewProj);
    return vout;
}

float4 LightVolumePS(VolumeVSOut pin) : SV_TARGET
{
    Surface s;
    if (!LoadSurface(pin.PosH, s))
        discard;
    return float4(EvaluateLight(gLights[pin.LightIndex], s), 1.f);
}
