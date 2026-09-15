#pragma once
#include <windows.h>
#include <DirectXMath.h>
#include <cstdint>
#include <memory>

class RenderingSystem;

class D3D12Context
{
public:
    D3D12Context();
    ~D3D12Context();

    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();

    void OnResize(uint32_t width, uint32_t height);
    void Draw(float dt);
    void SetCamera(const DirectX::XMFLOAT3& eyePos, float yaw, float pitch);

private:
    std::unique_ptr<RenderingSystem> m_renderer;
};
