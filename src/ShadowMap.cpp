#include "ShadowMap.h"

#include <stdexcept>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <cstring>

using namespace DirectX;

namespace
{
    void ThrowIfFailed(HRESULT hr, const char* what)
    {
        if (FAILED(hr))
        {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "%s (hr=0x%08X)", what, static_cast<unsigned>(hr));
            throw std::runtime_error(buffer);
        }
    }
}

void ShadowMap::Initialize(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE srvDest)
{
    // TYPELESS, чтобы одну и ту же память видеть как D32_FLOAT (DSV) и R32_FLOAT (SRV).
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = Size;
    desc.Height = Size;
    desc.DepthOrArraySize = CascadeCount;   // слайсы массива = каскады
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = GetDsvFormat();
    clear.DepthStencil.Depth = 1.f;

    ThrowIfFailed(
        device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
            IID_PPV_ARGS(&m_resource)),
        "Create shadow map array");
    m_resource->SetName(L"ShadowMapArray");

    // По одному DSV на каскад: рисуем в каждый слайс отдельно.
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = CascadeCount;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)), "Create shadow DSV heap");

    m_dsvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    for (uint32_t i = 0; i < CascadeCount; ++i)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = GetDsvFormat();
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.MipSlice = 0;
        dsv.Texture2DArray.FirstArraySlice = i;
        dsv.Texture2DArray.ArraySize = 1;
        device->CreateDepthStencilView(m_resource.Get(), &dsv, GetDsv(i));
    }

    // Константный буфер с матрицами каскадов (upload-heap, размер кратен 256).
    D3D12_HEAP_PROPERTIES upload = heap;
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC cbDesc{};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = (sizeof(ShadowConstants) + 255u) & ~255u;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ThrowIfFailed(
        device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_constantBuffer)),
        "Create shadow constant buffer");
    D3D12_RANGE noRead{ 0, 0 };
    ThrowIfFailed(m_constantBuffer->Map(0, &noRead, reinterpret_cast<void**>(&m_mappedConstants)), "Map shadow constant buffer");

    // Один SRV на весь массив: в шейдере Texture2DArray<float>, индекс = номер каскада.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = GetSrvFormat();
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Texture2DArray.MostDetailedMip = 0;
    srv.Texture2DArray.MipLevels = 1;
    srv.Texture2DArray.FirstArraySlice = 0;
    srv.Texture2DArray.ArraySize = CascadeCount;
    device->CreateShaderResourceView(m_resource.Get(), &srv, srvDest);
}

void ShadowMap::Shutdown()
{
    if (m_constantBuffer && m_mappedConstants)
    {
        m_constantBuffer->Unmap(0, nullptr);
        m_mappedConstants = nullptr;
    }
    m_constantBuffer.Reset();
    m_resource.Reset();
    m_dsvHeap.Reset();
}

D3D12_CPU_DESCRIPTOR_HANDLE ShadowMap::GetDsv(uint32_t cascade) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(cascade) * m_dsvDescriptorSize;
    return handle;
}

void ShadowMap::TransitionToWrite(ID3D12GraphicsCommandList* cmdList)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

void ShadowMap::TransitionToRead(ID3D12GraphicsCommandList* cmdList)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

void ShadowMap::UpdateCascades(const XMFLOAT4X4& view, float fovY, float aspect,
                               float camNear, float shadowFar, const XMFLOAT3& lightDir)
{
    // --- 1. Practical Split Scheme: смесь логарифмического и равномерного разбиения ---
    const float lambda = 0.75f;   // 0 = только равномерное, 1 = только логарифмическое
    float splits[CascadeCount + 1];
    splits[0] = camNear;
    for (uint32_t i = 1; i <= CascadeCount; ++i)
    {
        const float t = static_cast<float>(i) / CascadeCount;
        const float logSplit = camNear * std::pow(shadowFar / camNear, t);
        const float uniSplit = camNear + (shadowFar - camNear) * t;
        splits[i] = lambda * logSplit + (1.f - lambda) * uniSplit;
    }

    const XMMATRIX invView = XMMatrixInverse(nullptr, XMLoadFloat4x4(&view));
    const float tanHalfFov = std::tan(fovY * 0.5f);

    // Матрица "мир -> пространство света" с началом в (0,0,0): только поворот.
    // Позицию света не задаём: для ортографики важна лишь ориентация, сдвиг по X/Y делаем ниже сами.
    const XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&lightDir));
    const XMMATRIX lightView = XMMatrixLookToLH(XMVectorZero(), dir, XMVectorSet(0.f, 1.f, 0.f, 0.f));

    // Насколько дальше к источнику света от каскада могут находиться объекты, отбрасывающие на него тень.
    const float casterMargin = 50.f;

    for (uint32_t c = 0; c < CascadeCount; ++c)
    {
        // --- 2. 8 углов куска frustum камеры [splits[c], splits[c+1]] в мировых координатах ---
        XMVECTOR corners[8];
        XMVECTOR center = XMVectorZero();
        for (int k = 0; k < 8; ++k)
        {
            const float d = (k < 4) ? splits[c] : splits[c + 1];
            const float x = (k & 1) ? 1.f : -1.f;
            const float y = (k & 2) ? 1.f : -1.f;
            const XMVECTOR viewPos = XMVectorSet(x * tanHalfFov * aspect * d, y * tanHalfFov * d, d, 1.f);
            corners[k] = XMVector3TransformCoord(viewPos, invView);
            center = XMVectorAdd(center, corners[k]);
        }
        center = XMVectorScale(center, 1.f / 8.f);

        // --- 3. Ограничивающая сфера: радиус не зависит от поворота камеры -> размер тени стабилен ---
        float radius = 0.f;
        for (int k = 0; k < 8; ++k)
            radius = std::max(radius, XMVectorGetX(XMVector3Length(XMVectorSubtract(corners[k], center))));
        radius = std::ceil(radius * 16.f) / 16.f;   // округляем, чтобы радиус не "дрожал" от float-шума

        // --- 4. Texel snapping: сдвигаем центр окна на целое число текселей ---
        const float texelSize = 2.f * radius / static_cast<float>(Size);
        XMFLOAT3 c3;
        XMStoreFloat3(&c3, XMVector3TransformCoord(center, lightView));   // центр в пространстве света
        c3.x = std::floor(c3.x / texelSize) * texelSize;
        c3.y = std::floor(c3.y / texelSize) * texelSize;

        // --- 5. Ортографическая проекция света вокруг центра каскада ---
        const XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(
            c3.x - radius, c3.x + radius,
            c3.y - radius, c3.y + radius,
            c3.z - radius - casterMargin, c3.z + radius);

        XMStoreFloat4x4(&m_constants.LightViewProj[c], XMMatrixTranspose(lightView * lightProj));
        (&m_constants.TexelWorldSize.x)[c] = texelSize;
    }

    m_constants.CascadeSplits = XMFLOAT4(splits[1], splits[2], splits[3], splits[4]);

    if (m_mappedConstants)
        std::memcpy(m_mappedConstants, &m_constants, sizeof(m_constants));
}
