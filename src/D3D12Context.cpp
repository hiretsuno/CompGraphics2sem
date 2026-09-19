#include "D3D12Context.h"
#include "RenderingSystem.h"

D3D12Context::D3D12Context() = default;
D3D12Context::~D3D12Context() { Shutdown(); }

bool D3D12Context::Initialize(HWND hwnd, uint32_t width, uint32_t height)
{
    m_renderer = std::make_unique<RenderingSystem>();
    return m_renderer->Initialize(hwnd, width, height);
}

void D3D12Context::ToggleSceneMode()      { if (m_renderer) m_renderer->ToggleSceneMode(); }
void D3D12Context::ToggleFrustumCulling() { if (m_renderer) m_renderer->ToggleFrustumCulling(); }
void D3D12Context::ToggleOctreeCulling()  { if (m_renderer) m_renderer->ToggleOctreeCulling(); }
bool D3D12Context::FrustumCullingOn() const { return m_renderer ? m_renderer->FrustumCullingOn() : false; }
bool D3D12Context::OctreeCullingOn()  const { return m_renderer ? m_renderer->OctreeCullingOn()  : false; }
bool D3D12Context::ScatterModeOn()    const { return m_renderer ? m_renderer->ScatterModeOn()    : false; }
uint32_t D3D12Context::ScatterVisibleCount() const { return m_renderer ? m_renderer->ScatterVisibleCount() : 0; }
uint32_t D3D12Context::ScatterTotalCount()   const { return m_renderer ? m_renderer->ScatterTotalCount()   : 0; }

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
