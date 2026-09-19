#pragma once
#include <DirectXMath.h>
#include <algorithm>
#include <cfloat>

// Ограничивающий параллелепипед, выровненный по осям.
struct AABB
{
    DirectX::XMFLOAT3 Min{ FLT_MAX, FLT_MAX, FLT_MAX };
    DirectX::XMFLOAT3 Max{ -FLT_MAX, -FLT_MAX, -FLT_MAX };

    void Expand(const DirectX::XMFLOAT3& p)
    {
        Min.x = std::min(Min.x, p.x);
        Min.y = std::min(Min.y, p.y);
        Min.z = std::min(Min.z, p.z);
        Max.x = std::max(Max.x, p.x);
        Max.y = std::max(Max.y, p.y);
        Max.z = std::max(Max.z, p.z);
    }

    DirectX::XMFLOAT3 Center() const
    {
        return { (Min.x + Max.x) * 0.5f, (Min.y + Max.y) * 0.5f, (Min.z + Max.z) * 0.5f };
    }
};

// Преобразует локальный AABB в мировой через матрицу.
// Берём 8 углов, трансформируем их, получаем новый AABB.
AABB TransformAABB(const AABB& local, const DirectX::XMFLOAT4X4& world);