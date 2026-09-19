#include "Frustum.h"

using namespace DirectX;

AABB TransformAABB(const AABB& local, const XMFLOAT4X4& world)
{
    const XMVECTOR corners[8] = {
        XMVectorSet(local.Min.x, local.Min.y, local.Min.z, 1.f),
        XMVectorSet(local.Max.x, local.Min.y, local.Min.z, 1.f),
        XMVectorSet(local.Min.x, local.Max.y, local.Min.z, 1.f),
        XMVectorSet(local.Max.x, local.Max.y, local.Min.z, 1.f),
        XMVectorSet(local.Min.x, local.Min.y, local.Max.z, 1.f),
        XMVectorSet(local.Max.x, local.Min.y, local.Max.z, 1.f),
        XMVectorSet(local.Min.x, local.Max.y, local.Max.z, 1.f),
        XMVectorSet(local.Max.x, local.Max.y, local.Max.z, 1.f),
    };

    const XMMATRIX W = XMLoadFloat4x4(&world);
    AABB result;
    for (const auto& c : corners)
    {
        XMFLOAT4 t;
        XMStoreFloat4(&t, XMVector4Transform(c, W));
        result.Expand({ t.x, t.y, t.z });
    }
    return result;
}

// Извлечение плоскостей фрустума из View*Proj.
// Если в коде матрица хранится транспонированной (как у нас), то
// "строки" VP-матрицы = столбцы XMFLOAT4X4.
Frustum Frustum::FromViewProj(const XMFLOAT4X4& vp)
{
    // Строки VP-матрицы:
    const XMFLOAT4 r0 = { vp._11, vp._21, vp._31, vp._41 };
    const XMFLOAT4 r1 = { vp._12, vp._22, vp._32, vp._42 };
    const XMFLOAT4 r2 = { vp._13, vp._23, vp._33, vp._43 };
    const XMFLOAT4 r3 = { vp._14, vp._24, vp._34, vp._44 };

    auto add = [](const XMFLOAT4& a, const XMFLOAT4& b) {
        return XMFLOAT4{ a.x+b.x, a.y+b.y, a.z+b.z, a.w+b.w };
    };
    auto sub = [](const XMFLOAT4& a, const XMFLOAT4& b) {
        return XMFLOAT4{ a.x-b.x, a.y-b.y, a.z-b.z, a.w-b.w };
    };

    Frustum f;
    f.Planes[0].P = add(r3, r0); // left
    f.Planes[1].P = sub(r3, r0); // right
    f.Planes[2].P = add(r3, r1); // bottom
    f.Planes[3].P = sub(r3, r1); // top
    f.Planes[4].P = r2;          // near
    f.Planes[5].P = sub(r3, r2); // far

    // Нормализуем плоскости (чтобы расстояние было в мировых единицах).
    for (auto& p : f.Planes)
    {
        const float len = sqrtf(p.P.x*p.P.x + p.P.y*p.P.y + p.P.z*p.P.z);
        if (len > 1e-6f)
        {
            p.P.x /= len; p.P.y /= len; p.P.z /= len; p.P.w /= len;
        }
    }
    return f;
}

// Тест AABB vs фрустум: для каждой плоскости берём "самую дальнюю" точку
// AABB в направлении нормали. Если она внутри (>=0), AABB не полностью снаружи.
bool Frustum::Intersects(const AABB& aabb) const
{
    for (const auto& p : Planes)
    {
        // "Positive vertex" — точка AABB, наиболее удалённая в направлении нормали.
        const float px = (p.P.x >= 0.f) ? aabb.Max.x : aabb.Min.x;
        const float py = (p.P.y >= 0.f) ? aabb.Max.y : aabb.Min.y;
        const float pz = (p.P.z >= 0.f) ? aabb.Max.z : aabb.Min.z;

        if (p.P.x * px + p.P.y * py + p.P.z * pz + p.P.w < 0.f)
            return false; // AABB полностью снаружи этой плоскости
    }
    return true;
}