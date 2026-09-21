#pragma once

#include <wrl.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>

// Данные каскадов для шейдеров (лежат в константном буфере).
struct alignas(16) ShadowConstants
{
    DirectX::XMFLOAT4X4 LightViewProj[4];  // World -> свет-clip, УЖЕ транспонированы для HLSL
    DirectX::XMFLOAT4   CascadeSplits;     // x..w = дальняя граница каскада 0..3 (расстояние вдоль взгляда камеры)
    DirectX::XMFLOAT4   TexelWorldSize;    // x..w = размер текселя каскада в метрах (для bias в шейдере)
};

// Каскадная карта теней: один Texture2DArray глубины, по одному слайсу на каскад.
class ShadowMap
{
public:
    static constexpr uint32_t CascadeCount = 4;
    static constexpr uint32_t Size = 2048;   // разрешение одного каскада (Size x Size)

    // srvDest — куда записать SRV на весь массив (слот в куче GBuffer, см. GBuffer::GetShadowSrvCpu).
    void Initialize(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE srvDest);
    void Shutdown();

    // Ресурс живёт в PIXEL_SHADER_RESOURCE; на время shadow pass переводим в DEPTH_WRITE.
    void TransitionToWrite(ID3D12GraphicsCommandList* cmdList);
    void TransitionToRead(ID3D12GraphicsCommandList* cmdList);

    // Пересчитывает границы каскадов и LightViewProj, пишет их в константный буфер.
    //   view       — матрица вида камеры (row-vector, как в DirectXMath)
    //   camNear    — near камеры; shadowFar — до какого расстояния строим тени
    //   lightDir   — направление, КУДА светит солнце (от источника к сцене)
    void UpdateCascades(const DirectX::XMFLOAT4X4& view, float fovY, float aspect,
                        float camNear, float shadowFar, const DirectX::XMFLOAT3& lightDir);

    const ShadowConstants& GetConstants() const { return m_constants; }
    D3D12_GPU_VIRTUAL_ADDRESS GetConstantsGpuAddress() const { return m_constantBuffer->GetGPUVirtualAddress(); }

    D3D12_CPU_DESCRIPTOR_HANDLE GetDsv(uint32_t cascade) const;
    ID3D12Resource* GetResource() const { return m_resource.Get(); }

    DXGI_FORMAT GetDsvFormat() const { return DXGI_FORMAT_D32_FLOAT; }
    DXGI_FORMAT GetSrvFormat() const { return DXGI_FORMAT_R32_FLOAT; }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;   // CascadeCount DSV
    uint32_t m_dsvDescriptorSize = 0;

    ShadowConstants m_constants{};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_constantBuffer;   // upload, замаплен постоянно
    uint8_t* m_mappedConstants = nullptr;
};
