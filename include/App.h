#pragma once
#include <windows.h>
#include <cstdint>
#include "D3D12Context.h"

class Window;
class Input;

class App
{
public:
    bool Initialize(HINSTANCE hInstance, int nCmdShow);
    int Run();
    LRESULT HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

private:
    void Update(float dt);

    Window* m_window = nullptr;
    Input* m_input = nullptr;
    D3D12Context* m_dx12 = nullptr;

    bool m_exitRequested = false;
    uint64_t m_prevTick = 0;
    double m_secondsPerTick = 0.0;

    float m_camYaw = 1.f;
    float m_camPitch = 0.f;
    DirectX::XMFLOAT3 m_camPos{ -5.f, 1.f, -5.f };

    POINT m_savedCursorPos{ 0, 0 };
    bool m_justEnteredRmbLook = false;
};
