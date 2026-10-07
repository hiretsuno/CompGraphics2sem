#include "App.h"
#include <windows.h>
#include <cstdio>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    try
    {
        App app;
        if (!app.Initialize(hInstance, nCmdShow))
            return 0;
        return app.Run();
    }
    catch (const std::exception& e)
    {
        char buf[1024];
        std::snprintf(buf, sizeof(buf), "Exception: %s", e.what());
        MessageBoxA(nullptr, buf, "Fatal error", MB_OK | MB_ICONERROR);
        return 1;
    }
    catch (...)
    {
        MessageBoxA(nullptr, "Unknown exception", "Fatal error", MB_OK | MB_ICONERROR);
        return 2;
    }
}