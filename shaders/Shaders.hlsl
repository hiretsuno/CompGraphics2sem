Texture2D gDiffuseMap : register(t0);
SamplerState gSampler : register(s0);

cbuffer ObjectCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldViewProj;
    float3 gEyePosW; float _pad0;
    float3 gLightDirW; float _pad1;
    float4 gUvOffsetTiling;
    float gSpecPower; float3 _pad2;
};

cbuffer PerDrawCB : register(b1)
{
    float4 gKd;
};

struct VSIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

struct PSIn
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
};

PSIn VSMain(VSIn vin)
{
    PSIn vout;
    float4 posW = mul(float4(vin.PosL, 1.f), gWorld);
    vout.PosW = posW.xyz;
    vout.NormalW = normalize(mul(vin.NormalL, (float3x3)gWorld));
    vout.PosH = mul(float4(vin.PosL, 1.f), gWorldViewProj);
    vout.TexC = vin.TexC;
    return vout;
}

float4 PSMain(PSIn pin) : SV_TARGET
{
    float2 uv = pin.TexC * gUvOffsetTiling.zw + gUvOffsetTiling.xy;
    float3 albedo = gDiffuseMap.Sample(gSampler, uv).rgb * gKd.rgb;

    float3 N = normalize(pin.NormalW);
    float3 L = normalize(-gLightDirW);
    float3 V = normalize(gEyePosW - pin.PosW);
    float3 H = normalize(L + V);

    float ndotl = saturate(dot(N, L));
    float spec = pow(saturate(dot(N, H)), gSpecPower);

    float3 color = albedo*(0.5f + ndotl) + spec*0.25f;
    return float4(color, 1.f);
}
