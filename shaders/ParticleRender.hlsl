// Отрисовка частиц: POINTLIST -> VS (pass-through) -> GS (точка -> билборд) -> PS (непрозрачный круг).

// Тот же PassCB (b0), что в Shaders.hlsl: частицы уже в мировых координатах, gWorld не применяем.
cbuffer PassCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float4   gEyePosW;
    float4   gRTSize;
};

// Раскладка совпадает со struct Particle (32 байта), буфер частиц подключён как vertex buffer.
struct VSIn
{
    float3 Position : POSITION;
    float  Size     : SIZE;
    float3 Velocity : VELOCITY;
    float  Lifetime : LIFETIME;
};

struct VSOut
{
    float3 Position : POSITION;
    float  Size     : SIZE;
    float  Lifetime : LIFETIME;
};

struct GSOut
{
    float4 PosH  : SV_POSITION;
    float2 UV    : TEXCOORD;    // [-1, 1] внутри квадрата, для круглой маски
    float3 Color : COLOR;
};

// ---------------- Vertex Shader: просто передаём данные дальше ----------------
VSOut ParticleVS(VSIn vin)
{
    VSOut vout;
    vout.Position = vin.Position;
    vout.Size     = vin.Size;
    vout.Lifetime = vin.Lifetime;
    return vout;
}

// ---------------- Geometry Shader: точка -> квадрат, повёрнутый лицом к камере ----------------
[maxvertexcount(4)]
void ParticleGS(point VSOut input[1], inout TriangleStream<GSOut> stream)
{
    const VSOut p = input[0];

    // Оси билборда: normal смотрит на камеру, right/up лежат в плоскости квадрата.
    float3 toEye = normalize(gEyePosW.xyz - p.Position);
    float3 right = cross(float3(0.0f, 1.0f, 0.0f), toEye);
    // Если смотрим строго вверх/вниз, cross вырождается — берём любую перпендикулярную ось.
    right = (dot(right, right) < 1e-6f) ? float3(1.0f, 0.0f, 0.0f) : normalize(right);
    float3 up = cross(toEye, right);

    // Цвет по остатку жизни: свежие — жёлтые, к концу остывают в тёмно-красные.
    float  heat  = saturate(p.Lifetime / 4.0f);
    float3 color = lerp(float3(0.55f, 0.06f, 0.0f), float3(1.0f, 0.78f, 0.25f), heat);

    const float  h = p.Size * 0.5f;
    const float2 corners[4] = { float2(-1, -1), float2(-1, 1), float2(1, -1), float2(1, 1) };   // порядок под TriangleStrip

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float3 worldPos = p.Position + (right * corners[i].x + up * corners[i].y) * h;

        GSOut v;
        v.PosH  = mul(float4(worldPos, 1.0f), gViewProj);
        v.UV    = corners[i];
        v.Color = color;
        stream.Append(v);
    }
}

// ---------------- Pixel Shader: непрозрачный круг ----------------
float4 ParticlePS(GSOut pin) : SV_Target
{
    float r2 = dot(pin.UV, pin.UV);
    clip(1.0f - r2);                              // вне круга — отбрасываем пиксель (alpha test, без блендинга)

    float shade = 1.0f - 0.35f * r2;              // лёгкий объём: края чуть темнее
    return float4(pin.Color * shade, 1.0f);
}
