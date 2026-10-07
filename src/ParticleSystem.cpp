#include "ParticleSystem.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <stdexcept>
#include <cstdio>
#include <cstring>

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
        props.CreationNodeMask = 1;
        props.VisibleNodeMask = 1;
        return props;
    }

    D3D12_RESOURCE_DESC BufferDesc(UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;
        return desc;
    }

    // Создаёт upload-буфер и копирует в него данные.
    Microsoft::WRL::ComPtr<ID3D12Resource> CreateUpload(ID3D12Device* device, const void* data, UINT64 size)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        const auto heap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        const auto desc = BufferDesc(size);
        ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource)), "Create particle upload buffer");
        void* mapped = nullptr;
        D3D12_RANGE noRead{ 0, 0 };
        ThrowIfFailed(resource->Map(0, &noRead, &mapped), "Map particle upload buffer");
        std::memcpy(mapped, data, static_cast<size_t>(size));
        resource->Unmap(0, nullptr);
        return resource;
    }
}

void ParticleSystem::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, const std::wstring& computeShaderPath)
{
    //1. Два буфера: [ MaxParticles * Particle | ...выравнивание... | 4 байта счётчика ] 
    // Счётчик Append/Consume лежит в том же ресурсе; его смещение обязано быть кратно 4096
    // (D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT).
    const uint32_t dataSize = MaxParticles * sizeof(Particle);
    m_counterOffset = (dataSize + D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT - 1) & ~(D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT - 1);
    const UINT64 bufferSize = m_counterOffset + sizeof(uint32_t);

    const auto defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    const auto bufferDesc = BufferDesc(bufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    for (int i = 0; i < 2; ++i)
    {
        // Committed-ресурсы зануляются при создании => счётчик стартует с 0, частиц нет.
        ThrowIfFailed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_buffers[i])), "Create particle buffer");   // буферы всегда стартуют в COMMON
        m_buffers[i]->SetName(i == 0 ? L"ParticlesA" : L"ParticlesB");
    }

    // UAV со счётчиком: [A, B, B, A]
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 4;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_uavHeap)), "Create particle UAV heap");
    m_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_UNKNOWN;                       // structured buffer
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.FirstElement = 0;
    uav.Buffer.NumElements = MaxParticles;
    uav.Buffer.StructureByteStride = sizeof(Particle);
    uav.Buffer.CounterOffsetInBytes = m_counterOffset;      // включает скрытый счётчик => можно Append/Consume
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

    const int slotToBuffer[4] = { 0, 1, 1, 0 };
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_uavHeap->GetCPUDescriptorHandleForHeapStart();
    for (int slot = 0; slot < 4; ++slot)
    {
        ID3D12Resource* buffer = m_buffers[slotToBuffer[slot]].Get();
        device->CreateUnorderedAccessView(buffer, buffer /*ресурс счётчика*/, &uav, handle);
        handle.ptr += m_descriptorSize;
    }

    // Буфер аргументов Draw + command signature для ExecuteIndirect
    // D3D12_DRAW_ARGUMENTS = { VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation }.
    // Каждый кадр в VertexCountPerInstance копируется счётчик частиц, остальные поля остаются {1, 0, 0}.
    const auto argsDesc = BufferDesc(sizeof(D3D12_DRAW_ARGUMENTS));
    ThrowIfFailed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &argsDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_args)), "Create particle indirect args");
    m_args->SetName(L"ParticleDrawArgs");

    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC signatureDesc{};
    signatureDesc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    signatureDesc.NumArgumentDescs = 1;
    signatureDesc.pArgumentDescs = &argument;
    ThrowIfFailed(device->CreateCommandSignature(&signatureDesc, nullptr, IID_PPV_ARGS(&m_drawSignature)),
        "Create particle draw command signature");

    // Буфер с числом живых частиц (шейдер читает его как CBV)
    const auto countDesc = BufferDesc(256);
    ThrowIfFailed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &countDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_countBuffer)), "Create particle count buffer");
    m_countBuffer->SetName(L"ParticleAliveCount");

    // --- 3c. Compute: root signature + 2 PSO (Simulate, Emit) ---
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 2;                             // u0 = consume, u1 = append
    uavRange.BaseShaderRegister = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &uavRange;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;   // b0: SimCB
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = 12;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;               // b1: CountCB
    params[2].Descriptor.ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = params;
    Microsoft::WRL::ComPtr<ID3DBlob> rsBlob, errors;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errors)))
        throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "particle root signature");
    ThrowIfFailed(device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&m_computeRootSig)),
        "Create particle compute root signature");

    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    auto makePso = [&](const char* entry, Microsoft::WRL::ComPtr<ID3D12PipelineState>& pso)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> bytecode, compileErrors;
        const HRESULT hr = D3DCompileFromFile(computeShaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entry, "cs_5_0", compileFlags, 0, &bytecode, &compileErrors);
        if (FAILED(hr))
            throw std::runtime_error(compileErrors ? static_cast<const char*>(compileErrors->GetBufferPointer()) : "particle CS compile");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = m_computeRootSig.Get();
        desc.CS = { bytecode->GetBufferPointer(), bytecode->GetBufferSize() };
        ThrowIfFailed(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)), "Create particle compute PSO");
    };
    makePso("SimulateCS", m_simulatePSO);
    makePso("EmitCS", m_emitPSO);

    //4. Вспомогательные upload-буферы + одноразовая команда инициализации args
    const uint32_t zero = 0;
    m_zeroUpload = CreateUpload(device, &zero, sizeof(zero));
    const D3D12_DRAW_ARGUMENTS initialArgs = { 0, 1, 0, 0 };
    m_argsInitUpload = CreateUpload(device, &initialArgs, sizeof(initialArgs));

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmdList;
    ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Particle init allocator");
    ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&cmdList)), "Particle init list");

    // Обе части буферов частиц -> UAV (дальше кадр сам ведёт состояния через PrepareDraw/FinishFrame).
    for (int i = 0; i < 2; ++i)
        Transition(cmdList.Get(), m_buffers[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Буфер с числом живых: дальше живёт в состоянии "константный буфер".
    Transition(cmdList.Get(), m_countBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    // args: заполняем {0, 1, 0, 0}, затем -> INDIRECT_ARGUMENT.
    Transition(cmdList.Get(), m_args.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyBufferRegion(m_args.Get(), 0, m_argsInitUpload.Get(), 0, sizeof(D3D12_DRAW_ARGUMENTS));
    Transition(cmdList.Get(), m_args.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    ThrowIfFailed(cmdList->Close(), "Particle init close");

    ID3D12CommandList* lists[] = { cmdList.Get() };
    queue->ExecuteCommandLists(1, lists);

    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Particle init fence");
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    ThrowIfFailed(queue->Signal(fence.Get(), 1), "Particle init signal");
    ThrowIfFailed(fence->SetEventOnCompletion(1, event), "Particle init wait");
    WaitForSingleObject(event, INFINITE);
    CloseHandle(event);
    m_argsInitUpload.Reset();

    m_consumeIndex = 0;
}

void ParticleSystem::Shutdown()
{
    m_argsInitUpload.Reset();
    m_zeroUpload.Reset();
    m_simulatePSO.Reset();
    m_emitPSO.Reset();
    m_computeRootSig.Reset();
    m_countBuffer.Reset();
    m_drawSignature.Reset();
    m_args.Reset();
    m_uavHeap.Reset();
    m_buffers[0].Reset();
    m_buffers[1].Reset();
}

D3D12_GPU_DESCRIPTOR_HANDLE ParticleSystem::GetUavTable() const
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_uavHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(m_consumeIndex) * 2u * m_descriptorSize;   // слот 0 или 2
    return handle;
}

void ParticleSystem::Transition(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource,
                                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

void ParticleSystem::ResetAppendCounter(ID3D12GraphicsCommandList* cmdList)
{
    ID3D12Resource* append = m_buffers[AppendIndex()].Get();
    Transition(cmdList, append, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyBufferRegion(append, m_counterOffset, m_zeroUpload.Get(), 0, sizeof(uint32_t));
    Transition(cmdList, append, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void ParticleSystem::PrepareDraw(ID3D12GraphicsCommandList* cmdList)
{
    ID3D12Resource* append = m_buffers[AppendIndex()].Get();

    // Читать буфер как источник копии и как vertex buffer можно одновременно (оба состояния read-only).
    const D3D12_RESOURCE_STATES readState =
        D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    Transition(cmdList, append, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readState);

    // Счётчик частиц -> VertexCountPerInstance (первые 4 байта args). Это и есть CopyStructureCount.
    Transition(cmdList, m_args.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyBufferRegion(m_args.Get(), 0, append, m_counterOffset, sizeof(uint32_t));
    Transition(cmdList, m_args.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
}

void ParticleSystem::FinishFrame(ID3D12GraphicsCommandList* cmdList)
{
    ID3D12Resource* append = m_buffers[AppendIndex()].Get();
    const D3D12_RESOURCE_STATES readState =
        D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    Transition(cmdList, append, readState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    m_consumeIndex = AppendIndex();   // ping-pong: только что записанный буфер станет consume
}

D3D12_VERTEX_BUFFER_VIEW ParticleSystem::GetVertexBufferView() const
{
    D3D12_VERTEX_BUFFER_VIEW view{};
    view.BufferLocation = m_buffers[AppendIndex()]->GetGPUVirtualAddress();
    view.SizeInBytes = MaxParticles * sizeof(Particle);   // без области счётчика
    view.StrideInBytes = sizeof(Particle);
    return view;
}

void ParticleSystem::CopyConsumeCount(ID3D12GraphicsCommandList* cmdList)
{
    ID3D12Resource* consume = m_buffers[m_consumeIndex].Get();
    Transition(cmdList, consume, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(cmdList, m_countBuffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COPY_DEST);

    cmdList->CopyBufferRegion(m_countBuffer.Get(), 0, consume, m_counterOffset, sizeof(uint32_t));

    Transition(cmdList, consume, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cmdList, m_countBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
}

void ParticleSystem::Simulate(ID3D12GraphicsCommandList* cmdList, float dt)
{
    CopyConsumeCount(cmdList);
    ResetAppendCounter(cmdList);

    // Сколько частиц родить: rate * dt, дробный остаток копим (иначе при 60 fps и малом rate ничего не родится).
    // Потолок защищает от всплеска после долгой паузы (перетаскивание окна и т.п.).
    m_emitAccum += EmitRate * std::min(dt, 0.1f);
    const uint32_t emitCount = static_cast<uint32_t>(m_emitAccum);
    m_emitAccum -= static_cast<float>(emitCount);

    struct SimConstants
    {
        float dt; uint32_t emitCount; uint32_t seed; float floorY;
        DirectX::XMFLOAT3 emitterPos; float emitSpeed;
        DirectX::XMFLOAT3 gravity; float restitution;
    } constants = { dt, emitCount, m_frameIndex, FloorY, EmitterPos, EmitSpeed, Gravity, Restitution };
    static_assert(sizeof(SimConstants) == 12 * sizeof(uint32_t), "SimCB = 12 DWORD в root signature");

    ID3D12DescriptorHeap* heaps[] = { m_uavHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(m_computeRootSig.Get());
    cmdList->SetComputeRootDescriptorTable(0, GetUavTable());
    cmdList->SetComputeRoot32BitConstants(1, 12, &constants, 0);
    cmdList->SetComputeRootConstantBufferView(2, m_countBuffer->GetGPUVirtualAddress());

    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = m_buffers[AppendIndex()].Get();

    // 1) Обновляем существующие частицы. Потоков берём с запасом на весь буфер, лишние выходят сразу.
    cmdList->SetPipelineState(m_simulatePSO.Get());
    cmdList->Dispatch((MaxParticles + 63) / 64, 1, 1);
    cmdList->ResourceBarrier(1, &uavBarrier);

    // 2) Рождаем новые (в тот же append-буфер).
    if (emitCount > 0)
    {
        cmdList->SetPipelineState(m_emitPSO.Get());
        cmdList->Dispatch((emitCount + 63) / 64, 1, 1);
        cmdList->ResourceBarrier(1, &uavBarrier);
    }

    ++m_frameIndex;
}
