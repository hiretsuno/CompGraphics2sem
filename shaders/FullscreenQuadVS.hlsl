struct FSOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

// Один большой треугольник без vertex buffer: DrawInstanced(3, 1, 0, 0)
FSOut FullscreenVS(uint id : SV_VertexID)
{
    FSOut vout;
    // id: 0 -> (0,0), 1 -> (2,0), 2 -> (0,2)
    vout.TexC = float2((id << 1) & 2, id & 2);
    // UV [0,2] -> NDC: x = u*2-1, y = 1-v*2 (V растёт вниз, NDC Y вверх)
    vout.PosH = float4(vout.TexC * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);
    return vout;
}
