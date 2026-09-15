#include "D3D12Context.h"
#include "RenderingSystem.h"

D3D12Context::D3D12Context() = default;
D3D12Context::~D3D12Context() { Shutdown(); }

bool D3D12Context::Initialize(HWND hwnd, uint32_t width, uint32_t height)
{
    m_renderer = std::make_unique<RenderingSystem>();
    return m_renderer->Initialize(hwnd, width, height);
}

void D3D12Context::Shutdown()
{
    if (m_renderer)
    {
        m_renderer->Shutdown();
        m_renderer.reset();
    }
}

void D3D12Context::OnResize(uint32_t width, uint32_t height)
{
    if (m_renderer)
        m_renderer->OnResize(width, height);
}

void D3D12Context::Draw(float dt)
{
    if (m_renderer)
        m_renderer->Draw(dt);
}

void D3D12Context::SetCamera(const DirectX::XMFLOAT3& eyePos, float yaw, float pitch)
{
    if (m_renderer)
        m_renderer->SetCamera(eyePos, yaw, pitch);
}
