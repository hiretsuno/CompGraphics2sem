#pragma once
#include <windows.h>
#include <cstdint>
#include "D3D12Context.h"
#include <DirectXMath.h>

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

    float m_camYaw = 1.5708f;  // стартовый вид: вдоль длинной оси атриума (+X)
    float m_camPitch = -0.2f;
    DirectX::XMFLOAT3 m_camPos{ -4.f, 2.f, 0.f };   // эмиттер частиц в (2, 0, 0) — прямо перед камерой

    // Post-process: F1 = vignette, F2 = chromatic aberration, F3 = debug G-Buffer (albedo -> normal -> depth -> material -> off)
    // PBR: F4 = подмена материала (как есть -> хром -> шероховатый -> глянцевый), F5 = IBL-ambient вкл/выкл, F6 = прямой свет вкл/выкл
    bool m_vignetteOn = true;
    bool m_chromaOn = true;
    int  m_debugView = 0;
    int  m_pbrOverride = 0;
    bool m_iblOn = true;
    bool m_directOn = true;
    bool m_prevF[6]{};

    POINT m_savedCursorPos{ 0, 0 };
    bool m_justEnteredRmbLook = false;
};
