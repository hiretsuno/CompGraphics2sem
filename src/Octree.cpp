#include "Octree.h"
#include <array>

namespace
{
    bool ContainsAABB(const AABB& outer, const AABB& inner)
    {
        return inner.Min.x >= outer.Min.x && inner.Max.x <= outer.Max.x
            && inner.Min.y >= outer.Min.y && inner.Max.y <= outer.Max.y
            && inner.Min.z >= outer.Min.z && inner.Max.z <= outer.Max.z;
    }

    // Делит AABB на 8 равных октантов.
    std::array<AABB, 8> SplitBounds(const AABB& b)
    {
        const float mx = (b.Min.x + b.Max.x) * 0.5f;
        const float my = (b.Min.y + b.Max.y) * 0.5f;
        const float mz = (b.Min.z + b.Max.z) * 0.5f;

        return {{
            {{b.Min.x, b.Min.y, b.Min.z}, {mx,      my,      mz}},
            {{mx,      b.Min.y, b.Min.z}, {b.Max.x, my,      mz}},
            {{b.Min.x, my,      b.Min.z}, {mx,      b.Max.y, mz}},
            {{mx,      my,      b.Min.z}, {b.Max.x, b.Max.y, mz}},
            {{b.Min.x, b.Min.y, mz},      {mx,      my,      b.Max.z}},
            {{mx,      b.Min.y, mz},      {b.Max.x, my,      b.Max.z}},
            {{b.Min.x, my,      mz},      {mx,      b.Max.y, b.Max.z}},
            {{mx,      my,      mz},      {b.Max.x, b.Max.y, b.Max.z}},
        }};
    }

    int FindContainingChild(const std::array<AABB, 8>& children, const AABB& obj)
    {
        for (int i = 0; i < 8; ++i)
            if (ContainsAABB(children[i], obj)) return i;
        return -1;
    }
}

void Octree::Build(const std::vector<AABB>& worldBounds,
                   const AABB& sceneBounds,
                   int maxDepth, int minPerLeaf)
{
    m_nodes.clear();
    m_objectBounds = worldBounds;
    if (worldBounds.empty()) return;

    Node root;
    root.Bounds = sceneBounds;
    root.Objects.resize(worldBounds.size());
    for (uint32_t i = 0; i < worldBounds.size(); ++i) root.Objects[i] = i;

    m_nodes.push_back(std::move(root));
    Subdivide(0, worldBounds, 0, maxDepth, minPerLeaf);
}

void Octree::Subdivide(int idx, const std::vector<AABB>& bounds,
                       int depth, int maxDepth, int minPerLeaf)
{
    if (depth >= maxDepth) return;
    if ((int)m_nodes[idx].Objects.size() <= minPerLeaf) return;

    const auto childBounds = SplitBounds(m_nodes[idx].Bounds);
    std::array<std::vector<uint32_t>, 8> childObjects;
    std::vector<uint32_t> retained;
    retained.reserve(m_nodes[idx].Objects.size());

    // Раскидываем объекты по октантам. Объекты, которые не влезают
    // целиком ни в один октант, остаются в текущем узле.
    for (uint32_t objIdx : m_nodes[idx].Objects)
    {
        const int c = FindContainingChild(childBounds, bounds[objIdx]);
        if (c == -1) retained.push_back(objIdx);
        else         childObjects[c].push_back(objIdx);
    }
    m_nodes[idx].Objects = std::move(retained);

    for (int c = 0; c < 8; ++c)
    {
        if (childObjects[c].empty()) continue;

        Node child;
        child.Bounds = childBounds[c];
        child.Objects = std::move(childObjects[c]);

        m_nodes[idx].Children[c] = (int)m_nodes.size();
        m_nodes.push_back(std::move(child));
    }

    // Рекурсия по детям.
    for (int c = 0; c < 8; ++c)
    {
        const int childIdx = m_nodes[idx].Children[c];
        if (childIdx != -1)
            Subdivide(childIdx, bounds, depth + 1, maxDepth, minPerLeaf);
    }
}

void Octree::QueryVisible(const Frustum& frustum, std::vector<uint32_t>& out) const
{
    out.clear();
    if (m_nodes.empty()) return;
    QueryNode(0, frustum, out);
}

void Octree::QueryNode(int idx, const Frustum& f, std::vector<uint32_t>& out) const
{
    const Node& n = m_nodes[idx];

    // Если узел снаружи фрустума — всё поддерево снаружи, выходим.
    if (!f.Intersects(n.Bounds)) return;

    // Проверяем каждый объект в узле.
    for (uint32_t objIdx : n.Objects)
        if (f.Intersects(m_objectBounds[objIdx]))
            out.push_back(objIdx);

    if (n.IsLeaf()) return;
    for (int c : n.Children)
        if (c != -1) QueryNode(c, f, out);
}