#pragma once
#include <DirectXMath.h>
#include "AABB.h"

// Плоскость вида ax + by + cz + d = 0. Нормаль смотрит внутрь фрустума.
struct Plane
{
    DirectX::XMFLOAT4 P;
};

// Пирамида видимости камеры — 6 плоскостей (left/right/bottom/top/near/far).
struct Frustum
{
    Plane Planes[6];

    // Извлекает плоскости из матрицы View*Proj (метод Gribb & Hartmann).
    static Frustum FromViewProj(const DirectX::XMFLOAT4X4& viewProj);

    // Проверяет, пересекается ли AABB с фрустумом.
    // false = объект полностью снаружи, можно не рисовать.
    bool Intersects(const AABB& aabb) const;
};