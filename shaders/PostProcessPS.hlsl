cbuffer PostCB : register(b0)
{
    float4 gRTSize;   // x=width, y=height, z=1/width, w=1/height
    float4 gVignette; // x = intensity [0..1], y = radius [0..1] (где начинается затемнение)
    float4 gChroma;   // x = strength (в UV, ~0.006)
    float4 gFlags;    // x = vignette, y = chroma, z = unused, w = debug view (0=off,1=albedo,2=normal,3=depth,4=material)
};

// Порядок = порядку дескрипторов в SRV-куче GBuffer: слоты 0..3 = G-Buffer, слот 4 = shadow map (тут не нужен), слот 5 = SceneColor
Texture2D gAlbedoSpecTex  : register(t0);   // rgb = albedo
Texture2D gNormalTex      : register(t1);   // xyz = world normal
Texture2D gDepthTex       : register(t2);   // R32_FLOAT, NDC depth
Texture2D gMaterialTex    : register(t3);   // r = metallic, g = roughness, b = ao
Texture2D gSceneColor     : register(t4);   // R8G8B8A8, уже освещённая сцена (слот 5 кучи, см. root signature)

SamplerState gLinearClamp : register(s0);

struct FSOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

float4 PostProcessPS(FSOut pin) : SV_TARGET
{
    float2 uv = pin.TexC;
    int3 coords = int3((int2)pin.PosH.xy, 0);   // Load: без фильтрации, для данных G-Buffer

    // ---- Отладочный просмотр G-Buffer (чтение и распаковка данных) ----
    if (gFlags.w > 0.5f)
    {
        if (gFlags.w < 1.5f)
            return float4(gAlbedoSpecTex.Load(coords).rgb, 1.f);
        if (gFlags.w < 2.5f)
            return float4(normalize(gNormalTex.Load(coords).xyz) * 0.5f + 0.5f, 1.f);
        if (gFlags.w < 3.5f)
            return float4(pow(saturate(gDepthTex.Load(coords).r), 50.f).xxx, 1.f);   // NDC-глубина нелинейна, степень растягивает near
        return float4(gMaterialTex.Load(coords).rgb, 1.f);   // R = metallic, G = roughness, B = ao
    }

    // ---- Эффект 1: хроматическая аберрация (сдвиг RGB от центра к краям) ----
    float3 color;
    if (gFlags.y > 0.5f)
    {
        float2 off = (uv - 0.5f) * gChroma.x;   // растёт к краям, в центре ноль
        color.r = gSceneColor.Sample(gLinearClamp, uv + off).r;
        color.g = gSceneColor.Sample(gLinearClamp, uv).g;
        color.b = gSceneColor.Sample(gLinearClamp, uv - off).b;
    }
    else
    {
        color = gSceneColor.Sample(gLinearClamp, uv).rgb;
    }

    // ---- Эффект 2: виньетка (затемнение к краям) ----
    if (gFlags.x > 0.5f)
    {
        float d   = length(uv - 0.5f) * 1.41421356f;   // 0 в центре, 1 в углу
        float vig = smoothstep(gVignette.y, 1.f, d);   // 0 внутри радиуса -> 1 в углу
        color *= 1.f - vig * gVignette.x;
    }

    return float4(saturate(color), 1.f);
}
