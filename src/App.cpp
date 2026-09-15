#include "App.h"
#include "Window.h"
#include "Input.h"
#include <windows.h>
#include <windowsx.h>
#include <algorithm>
#include <DirectXMath.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

static uint64_t GetQpc() { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }
static double GetQpf() { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return (double)f.QuadPart; }

bool App::Initialize(HINSTANCE hInstance, int nCmdShow)
{
    m_window = new Window();
    m_input = new Input();
    m_input->Reset();

    if (!m_window->Create(this, hInstance, nCmdShow, 1280, 720, L"Lab-5"))
        return false;

    m_secondsPerTick = 1./GetQpf();
    m_prevTick = GetQpc();

    m_dx12 = new D3D12Context();

    RECT rc{};
    GetClientRect(m_window->GetHwnd(), &rc);
    if (!m_dx12->Initialize(m_window->GetHwnd(),
        (uint32_t)(rc.right - rc.left), (uint32_t)(rc.bottom - rc.top)))
        return false;

    return true;
}

int App::Run()
{
    MSG msg{};
    while (!m_exitRequested)
    {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { m_exitRequested = true; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        uint64_t now = GetQpc();
        float dt = (float)((now - m_prevTick) * m_secondsPerTick);
        m_prevTick = now;

        Update(dt);
        if (m_dx12) m_dx12->Draw(dt);
    }
    return 0;
}

void App::Update(float dt)
{
    using namespace DirectX;

    if (m_input && m_input->IsKeyDown(VK_ESCAPE))
        m_exitRequested = true;

    if (!m_input || !m_dx12 || !m_window) return;

    if (m_input->IsKeyDown(VK_RBUTTON))
    {
        HWND hwnd = m_window->GetHwnd();
        RECT rc{}; GetClientRect(hwnd, &rc);
        POINT center{ (rc.right-rc.left)/2, (rc.bottom-rc.top)/2 };
        POINT centerScreen = center;
        ClientToScreen(hwnd, &centerScreen);

        if (m_justEnteredRmbLook)
        {
            SetCursorPos(centerScreen.x, centerScreen.y);
            m_justEnteredRmbLook = false;
            return;
        }

        POINT cur{}; GetCursorPos(&cur);
        m_camYaw += (cur.x - centerScreen.x) * 0.005f;
        m_camPitch -= (cur.y - centerScreen.y) * 0.005f;
        m_camPitch = std::clamp(m_camPitch, -(XM_PIDIV2 - 0.1f), XM_PIDIV2 - 0.1f);
        SetCursorPos(centerScreen.x, centerScreen.y);
    }

    float speed = m_input->IsKeyDown(VK_SHIFT) ? 12.f : 5.f;

    XMVECTOR fwd = XMVector3Normalize(XMVectorSet(sinf(m_camYaw), 0, cosf(m_camYaw), 0));
    XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0,1,0,0), fwd));
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    XMVECTOR move  = XMVectorZero();

    if (m_input->IsKeyDown('W')) move = XMVectorAdd(move, fwd);
    if (m_input->IsKeyDown('A')) move = XMVectorSubtract(move, right);
    if (m_input->IsKeyDown('S')) move = XMVectorSubtract(move, fwd);
    if (m_input->IsKeyDown('D')) move = XMVectorAdd(move, right);
    if (m_input->IsKeyDown('E')) move = XMVectorAdd(move, up);
    if (m_input->IsKeyDown('Q')) move = XMVectorSubtract(move, up);

    if (!XMVector3Equal(move, XMVectorZero()))
    {
        XMVECTOR pos = XMLoadFloat3(&m_camPos);
        pos = XMVectorAdd(pos, XMVectorScale(XMVector3Normalize(move), speed * dt));
        XMStoreFloat3(&m_camPos, pos);
    }

    m_dx12->SetCamera(m_camPos, m_camYaw, m_camPitch);
}

LRESULT App::HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_CLOSE:
    case WM_DESTROY:
        m_exitRequested = true;
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN: if (m_input) m_input->OnKeyDown((uint32_t)wparam); return 0;
    case WM_KEYUP: if (m_input) m_input->OnKeyUp((uint32_t)wparam); return 0;

    case WM_RBUTTONDOWN:
        if (m_input) m_input->OnKeyDown(VK_RBUTTON);
        m_justEnteredRmbLook = true;
        GetCursorPos(&m_savedCursorPos);
        ShowCursor(FALSE);
        SetCapture(hwnd);
        return 0;

    case WM_RBUTTONUP:
        if (m_input) m_input->OnKeyUp(VK_RBUTTON);
        SetCursorPos(m_savedCursorPos.x, m_savedCursorPos.y);
        ShowCursor(TRUE);
        ReleaseCapture();
        return 0;

    case WM_MOUSEMOVE:
        if (m_input) m_input->OnMouseMove(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;

    case WM_SIZE:
    {
        uint32_t w = LOWORD(lparam), h = HIWORD(lparam);
        if (w && h && m_dx12) m_dx12->OnResize(w, h);
        return 0;
    }
    default: return DefWindowProc(hwnd, msg, wparam, lparam);
    }
}
