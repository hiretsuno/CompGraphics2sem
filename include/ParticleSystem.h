#pragma once

#include <wrl.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>
#include <string>

// Одна частица. ДОЛЖНА совпадать с struct Particle в shaders/Particle.hlsli (32 байта).
struct Particle
{
    DirectX::XMFLOAT3 Position;
    float             Size;
    DirectX::XMFLOAT3 Velocity;
    float             Lifetime;   // оставшееся время жизни, сек; <= 0 = мертва
};
static_assert(sizeof(Particle) == 32, "Particle должна совпадать с HLSL-структурой");

// GPU-система частиц: два буфера, которые каждый кадр меняются ролями (ping-pong).
//   consume-буфер — из него Compute Shader забирает живые частицы (ConsumeStructuredBuffer)
//   append-буфер  — в него CS пишет обновлённые и новые частицы (AppendStructuredBuffer)
// Число живых частиц хранится в "скрытом" счётчике внутри каждого буфера.
class ParticleSystem
{
public:
    static constexpr uint32_t MaxParticles = 65536;

    // Создаёт буферы, UAV, буфер аргументов для непрямого Draw и compute-пайплайн.
    // computeShaderPath — путь к ParticleCS.hlsl. Сама выполняет и ждёт свою init-команду.
    void Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, const std::wstring& computeShaderPath);
    void Shutdown();

    // --- Параметры эмиттера (можно менять в любой момент) ---
    DirectX::XMFLOAT3 EmitterPos{ 2.0f, 0.02f, 0.0f };
    float             EmitRate = 4000.0f;                     // частиц в секунду
    float             EmitSpeed = 5.0f;                       // скорость струи, м/с
    DirectX::XMFLOAT3 Gravity{ 0.0f, -9.8f, 0.0f };
    float             FloorY = 0.02f;                         // высота пола
    float             Restitution = 0.45f;                    // упругость отскока

    // Compute-проход кадра: считает живых, сбрасывает счётчик append-буфера, обновляет частицы
    // (Simulate) и рождает новые (Emit). После него вызывать PrepareDraw.
    void Simulate(ID3D12GraphicsCommandList* cmdList, float dt);

    // --- Ресурсы для compute-прохода ---
    ID3D12DescriptorHeap* GetUavHeap() const { return m_uavHeap.Get(); }
    // Таблица из 2 UAV: u0 = consume-буфер, u1 = append-буфер (для текущего кадра).
    D3D12_GPU_DESCRIPTOR_HANDLE GetUavTable() const;

    // Перед compute-проходом: обнуляет счётчик append-буфера (он должен быть в состоянии UAV).
    void ResetAppendCounter(ID3D12GraphicsCommandList* cmdList);

    // Перед compute-проходом: копирует счётчик consume-буфера (число живых частиц) в буфер для root CBV.
    void CopyConsumeCount(ID3D12GraphicsCommandList* cmdList);

    // После compute-прохода: копирует счётчик append-буфера в буфер аргументов Draw (CopyStructureCount)
    // и переводит append-буфер в COPY_SOURCE | VERTEX_AND_CONSTANT_BUFFER, чтобы он стал вершинным буфером.
    void PrepareDraw(ID3D12GraphicsCommandList* cmdList);

    // После отрисовки: возвращает буфер в UAV и меняет буферы ролями для следующего кадра.
    void FinishFrame(ID3D12GraphicsCommandList* cmdList);

    // --- Ресурсы для render-прохода (пока после PrepareDraw) ---
    // append-буфер как vertex buffer. Раскладка для InputLayout: POSITION(float3,0) SIZE(float,12) VELOCITY(float3,16) LIFETIME(float,28).
    D3D12_VERTEX_BUFFER_VIEW GetVertexBufferView() const;
    ID3D12Resource* GetIndirectArgs() const { return m_args.Get(); }
    ID3D12CommandSignature* GetDrawSignature() const { return m_drawSignature.Get(); }

    ID3D12Resource* GetBuffer(uint32_t i) const { return m_buffers[i].Get(); }
    uint32_t CounterOffset() const { return m_counterOffset; }
    uint32_t AppendIndex() const { return 1u - m_consumeIndex; }

private:
    void Transition(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_buffers[2];     // данные + счётчик в конце буфера
    uint32_t m_counterOffset = 0;                            // смещение счётчика (кратно 4096)
    uint32_t m_consumeIndex = 0;                             // какой буфер сейчас consume, другой — append

    // 4 UAV: [A, B, B, A]. Таблица со слота 0 = (consume A, append B), со слота 2 = (consume B, append A).
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_uavHeap;
    uint32_t m_descriptorSize = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_args;           // D3D12_DRAW_ARGUMENTS: {число частиц, 1, 0, 0}
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_drawSignature;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_countBuffer;    // число живых частиц (читается шейдером как CBV)
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_computeRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_simulatePSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_emitPSO;
    float    m_emitAccum = 0.0f;                             // дробный остаток частиц для эмиттера
    uint32_t m_frameIndex = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_zeroUpload;     // 4 нулевых байта для сброса счётчика
    Microsoft::WRL::ComPtr<ID3D12Resource> m_argsInitUpload; // {0,1,0,0} для первичной инициализации args
};
