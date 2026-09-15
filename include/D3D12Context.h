#pragma once

#include <windows.h>
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <cstdint>
#include <vector>
#include <string>

class D3D12Context
{
public:
    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();

    void OnResize(uint32_t width, uint32_t height);
    void Draw(float dt);
    void SetCamera(const DirectX::XMFLOAT3& eyePos, float yaw, float pitch);

    struct Vertex
    {
        DirectX::XMFLOAT3 Pos;
        DirectX::XMFLOAT3 Normal;
        DirectX::XMFLOAT2 TexC;
    };

    struct DrawItem
    {
        uint32_t IndexCount = 0;
        uint32_t StartIndexLocation = 0;
        uint32_t TextureSrvIndex = 1;
        DirectX::XMFLOAT3 Kd{ 1.f, 1.f, 1.f };
    };

private:
    bool CreateDevice();
    bool CreateCommandObjects();
    bool CreateSwapChain();
    bool CreateDescriptorHeaps();
    bool CreateRtvForBackBuffers();
    bool CreateDepthStencil();

    bool BuildShaders();
    bool BuildGeometry();
    bool BuildConstantBuffer();
    bool BuildRootSignature();
    bool BuildPSO();
    void UpdateConstantBuffer();

    void FlushCommandQueue();

    D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferRTV() const;
    ID3D12Resource* CurrentBackBuffer() const;

private:
    static constexpr uint32_t kSwapChainBufferCount = 2;

    struct alignas(16) ObjectConstants
    {
        DirectX::XMFLOAT4X4 World;
        DirectX::XMFLOAT4X4 WorldViewProj;
        DirectX::XMFLOAT3 EyePosW;  float _pad0 = 0.f;
        DirectX::XMFLOAT3 LightDirW; float _pad1 = 0.f;
        DirectX::XMFLOAT4 UvOffsetTiling;
        float SpecPower = 32.f;
        float _pad2[3] = {};
    };

    bool m_initialized = false;
    HWND m_hwnd = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    Microsoft::WRL::ComPtr<IDXGIFactory4> m_factory;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_cmdQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_cmdAlloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmdList;

    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    uint64_t m_fenceValue = 0;
    HANDLE m_fenceEvent = nullptr;

    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_swapChainBuffers[kSwapChainBufferCount];
    uint32_t m_currBackBuffer = 0;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_cbvHeap;

    uint32_t m_rtvDescriptorSize = 0;
    uint32_t m_cbvSrvUavDescriptorSize = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthStencilBuffer;
    D3D12_VIEWPORT m_viewport{};
    D3D12_RECT m_scissorRect{};

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    Microsoft::WRL::ComPtr<ID3DBlob> m_vsBytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> m_psBytecode;

    D3D12_INPUT_ELEMENT_DESC m_inputLayout[3]{};

    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBufferGPU;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBufferGPU;
    D3D12_VERTEX_BUFFER_VIEW m_vbv{};
    D3D12_INDEX_BUFFER_VIEW m_ibv{};

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_textures;
    std::vector<DrawItem> m_drawItems;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_objectCB;
    uint8_t* m_mappedObjectCB = nullptr;
    uint32_t m_objectCBByteSize = 0;

    DirectX::XMFLOAT4X4 m_world{};
    DirectX::XMFLOAT4X4 m_view{};
    DirectX::XMFLOAT4X4 m_proj{};
    DirectX::XMFLOAT3 m_eyePos{ 0.5f, 4.f, -5.f };
    DirectX::XMFLOAT3 m_lightDir{ 0.8f, -0.5f, 0.4f };

    float m_time = 0.f;
    DirectX::XMFLOAT2 m_uvOffset{ 0.f, 0.f };
    DirectX::XMFLOAT2 m_uvTiling{ 1.f, 1.f };
};
