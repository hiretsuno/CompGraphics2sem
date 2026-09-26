#include "GBuffer.h"

#include <stdexcept>
#include <cstdio>

using Microsoft::WRL::ComPtr;

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

    D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type)
    {
        D3D12_HEAP_PROPERTIES props{};
        props.Type = type;
        props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        props.CreationNodeMask = 1;
        props.VisibleNodeMask = 1;
        return props;
    }

    D3D12_RESOURCE_DESC TextureDesc2D(uint32_t width, uint32_t height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = flags;
        return desc;
    }

    D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        return barrier;
    }

    const float ClearAlbedo[4] = { 0.f, 0.f, 0.f, 0.f };
    const float ClearNormal[4] = { 0.f, 0.f, 1.f, 0.f };
}

bool GBuffer::Initialize(ID3D12Device* device, uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;
    m_rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_dsvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = TargetCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)), "Create GBuffer RTV heap");

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.NumDescriptors = SrvCount;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap)), "Create GBuffer SRV heap");

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = 2;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)), "Create GBuffer DSV heap");

    CreateResources(device);
    return true;
}

void GBuffer::Shutdown()
{
    ReleaseResources();
    m_dsvHeap.Reset();
    m_srvHeap.Reset();
    m_rtvHeap.Reset();
    m_width = 0;
    m_height = 0;
    m_isWriteState = false;
}

void GBuffer::Resize(ID3D12Device* device, uint32_t width, uint32_t height)
{
    if (!width || !height)
        return;

    m_width = width;
    m_height = height;
    ReleaseResources();
    CreateResources(device);
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::GetDsv() const
{
    return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::GetReadOnlyDsv() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += m_dsvDescriptorSize;
    return handle;
}

void GBuffer::TransitionToWrite(ID3D12GraphicsCommandList* cmdList)
{
    if (m_isWriteState)
        return;

    D3D12_RESOURCE_BARRIER barriers[TargetCount + 1]{};
    for (uint32_t i = 0; i < TargetCount; ++i)
        barriers[i] = Transition(m_targets[i].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barriers[TargetCount] = Transition(m_depthStencil.Get(), DepthReadState, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    cmdList->ResourceBarrier(TargetCount + 1, barriers);
    m_isWriteState = true;
}

void GBuffer::TransitionToRead(ID3D12GraphicsCommandList* cmdList)
{
    if (!m_isWriteState)
        return;

    D3D12_RESOURCE_BARRIER barriers[TargetCount + 1]{};
    for (uint32_t i = 0; i < TargetCount; ++i)
        barriers[i] = Transition(m_targets[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barriers[TargetCount] = Transition(m_depthStencil.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, DepthReadState);

    cmdList->ResourceBarrier(TargetCount + 1, barriers);
    m_isWriteState = false;
}

void GBuffer::BindForGeometryPass(ID3D12GraphicsCommandList* cmdList)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[TargetCount]{};
    auto rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < TargetCount; ++i)
    {
        rtvs[i] = rtv;
        rtv.ptr += m_rtvDescriptorSize;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dsv = GetDsv();
    cmdList->OMSetRenderTargets(TargetCount, rtvs, FALSE, &dsv);
    cmdList->ClearRenderTargetView(rtvs[0], ClearAlbedo, 0, nullptr);
    cmdList->ClearRenderTargetView(rtvs[1], ClearNormal, 0, nullptr);
    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);
}

void GBuffer::CreateResources(ID3D12Device* device)
{
    auto defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);

    const DXGI_FORMAT formats[TargetCount] = {
        GetAlbedoSpecFormat(),
        GetNormalFormat()
    };
    const float* clearColors[TargetCount] = { ClearAlbedo, ClearNormal };

    for (uint32_t i = 0; i < TargetCount; ++i)
    {
        D3D12_CLEAR_VALUE clear{};
        clear.Format = formats[i];
        for (int c = 0; c < 4; ++c)
            clear.Color[c] = clearColors[i][c];

        auto desc = TextureDesc2D(m_width, m_height, formats[i], D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        ThrowIfFailed(
            device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                &clear,
                IID_PPV_ARGS(&m_targets[i])),
            "Create GBuffer target");
    }

    // Depth создаётся typeless, чтобы на один ресурс можно было сделать и DSV (D32_FLOAT), и SRV (R32_FLOAT)
    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = GetDepthStencilFormat();
    depthClear.DepthStencil.Depth = 1.f;
    depthClear.DepthStencil.Stencil = 0;

    auto depthDesc = TextureDesc2D(m_width, m_height, GetDepthResourceFormat(), D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    ThrowIfFailed(
        device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &depthDesc,
            DepthReadState,
            &depthClear,
            IID_PPV_ARGS(&m_depthStencil)),
        "Create GBuffer depth stencil");

    auto rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < TargetCount; ++i)
    {
        device->CreateRenderTargetView(m_targets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = GetDepthStencilFormat();
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    device->CreateDepthStencilView(m_depthStencil.Get(), &dsvDesc, GetDsv());

    dsvDesc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
    device->CreateDepthStencilView(m_depthStencil.Get(), &dsvDesc, GetReadOnlyDsv());

    auto srvHandle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    const uint32_t srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    for (uint32_t i = 0; i < TargetCount; ++i)
    {
        srvDesc.Format = formats[i];
        device->CreateShaderResourceView(m_targets[i].Get(), &srvDesc, srvHandle);
        srvHandle.ptr += srvDescriptorSize;
    }

    srvDesc.Format = GetDepthSrvFormat();
    device->CreateShaderResourceView(m_depthStencil.Get(), &srvDesc, srvHandle);

    m_isWriteState = false;
}

void GBuffer::ReleaseResources()
{
    for (auto& target : m_targets)
        target.Reset();
    m_depthStencil.Reset();
}
