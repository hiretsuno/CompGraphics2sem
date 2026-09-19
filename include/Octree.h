#pragma once
#include "Frustum.h"
#include <vector>
#include <cstdint>

// Окто-дерево: каждый узел делит пространство на 8 октантов.
// Используется для быстрого отсечения: если узел снаружи фрустума,
// всё его поддерево тоже снаружи (не нужно проверять объекты).
class Octree
{
public:
    struct Node
    {
        AABB Bounds;                       // границы узла
        std::vector<uint32_t> Objects;     // индексы объектов, "сидящих" в этом узле
        int Children[8] = { -1,-1,-1,-1,-1,-1,-1,-1 };

        bool IsLeaf() const
        {
            for (int c : Children) if (c != -1) return false;
            return true;
        }
    };

    // worldBounds — мировой AABB каждого объекта.
    // sceneBounds — корневой AABB, покрывающий всю сцену.
    // maxDepth — максимальная глубина рекурсии.
    // minPerLeaf — если объектов в узле <= этого числа, дальше не делим.
    void Build(const std::vector<AABB>& worldBounds,
               const AABB& sceneBounds,
               int maxDepth = 6,
               int minPerLeaf = 8);

    // Заполняет out индексами объектов, чей AABB пересекается с фрустумом.
    void QueryVisible(const Frustum& frustum, std::vector<uint32_t>& out) const;

    size_t NodeCount() const { return m_nodes.size(); }
    size_t ObjectCount() const { return m_objectBounds.size(); }

private:
    void Subdivide(int idx, const std::vector<AABB>& bounds,
                   int depth, int maxDepth, int minPerLeaf);
    void QueryNode(int idx, const Frustum& f, std::vector<uint32_t>& out) const;

    std::vector<Node> m_nodes;
    std::vector<AABB> m_objectBounds;
};