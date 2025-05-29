#include "Overlay.h"
#include <iostream>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    // Create and initialize our overlay
    Overlay overlay;
    if (!overlay.Initialize())
    {
        MessageBoxW(NULL, L"Failed to initialize overlay", L"Error", MB_OK | MB_ICONERROR);
        return -1;
    }
    
    // Main loop
    overlay.Run();
    
    return 0;
}