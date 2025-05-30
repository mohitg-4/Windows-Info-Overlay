#include "Overlay.h"
#include <dwmapi.h>
#include <iostream>
#include <iphlpapi.h>
#include <vector>
#include <pdh.h>
#include <wbemidl.h>
#include <comdef.h>
#include <queue>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <shlobj.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "Ole32.lib")

// Global variables for the keyboard hook
HHOOK g_keyboardHook = NULL;
HWND g_overlayHwnd = NULL;

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Keyboard hook procedure
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    static bool altKeyPressed = false;
    
    if (nCode == HC_ACTION)
    {
        KBDLLHOOKSTRUCT* kbStruct = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        
        // Check for Alt key (VK_LMENU or VK_RMENU)
        if (kbStruct->vkCode == VK_LMENU || kbStruct->vkCode == VK_RMENU)
        {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
            {
                altKeyPressed = true;
            }
            else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
            {
                altKeyPressed = false;
            }
        }
        // Check for Space key while Alt is pressed
        else if (altKeyPressed && kbStruct->vkCode == VK_SPACE)
        {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
            {
                // Send message to toggle overlay
                if (g_overlayHwnd)
                {
                    PostMessage(g_overlayHwnd, WM_TOGGLE_OVERLAY, 0, 0);
                }
            }
        }
        // Check for Enter key to close the overlay when it's visible
        else if (kbStruct->vkCode == VK_RETURN)
        {
            if (wParam == WM_KEYDOWN)
            {
                // Send message to toggle off overlay if it's currently visible
                if (g_overlayHwnd)
                {
                    // We'll use a different message specifically for hiding
                    PostMessage(g_overlayHwnd, WM_TOGGLE_OVERLAY, 1, 0);
                }
            }
        }
    }
    
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    // Declare overlay pointer outside of the switch statement
    Overlay* overlay = nullptr;
    
    switch (msg)
    {
    case WM_TOGGLE_OVERLAY:
        {
            overlay = reinterpret_cast<Overlay*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
            if (overlay)
            {
                // If wParam is 1, we're specifically hiding the overlay
                if (wParam == 1 && overlay->m_isVisible)
                {
                    overlay->Toggle();
                }
                // Otherwise, it's a regular toggle
                else if (wParam == 0)
                {
                    overlay->Toggle();
                }
            }
            return 0;
        }
    case WM_SIZE:
        {
            overlay = reinterpret_cast<Overlay*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
            if (overlay && overlay->m_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
            {
                overlay->CleanupRenderTarget();
                overlay->m_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                overlay->CreateRenderTarget();
            }
            return 0;
        }
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

Overlay::Overlay() : 
    m_hwnd(nullptr), 
    m_pd3dDevice(nullptr), 
    m_pd3dDeviceContext(nullptr), 
    m_pSwapChain(nullptr), 
    m_mainRenderTargetView(nullptr), 
    m_isRunning(false), 
    m_isVisible(false),
    m_cpuQuery(NULL), 
    m_cpuTotal(NULL), 
    m_pdh_initialized(false), 
    m_smoothedCpuUsage(0),
    m_showSettings(false),
    m_showAudioWindow(false),
    m_showNetworkWindow(false),
    m_showAudioSettings(false),
    m_showNetworkSettings(false),
    m_mouseInsideAudioWindow(false),
    m_mouseInsideNetworkWindow(false)
{
    // Initialize default settings
    m_settings.audioSettings.showVolumePercentage = true;
    m_settings.audioSettings.showDeviceSelector = true;
    m_settings.audioSettings.alwaysOnTop = false;
    m_settings.audioSettings.savePosition = true;
    
    m_settings.networkSettings.showNetworkDetails = true;
    m_settings.networkSettings.alwaysOnTop = false;
    m_settings.networkSettings.savePosition = true;
    
    // Initialize PDH
    InitializeCpuCounter();
    
    // AudioManager and NetworkManager are automatically initialized by their constructors
}

Overlay::~Overlay()
{
    Cleanup();
}

bool Overlay::Initialize()
{
    // Create application window
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), 
                     nullptr, nullptr, nullptr, nullptr, L"WindowsInfoOverlay", nullptr };
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowW(wc.lpszClassName, L"Windows Info Overlay", 
                         WS_POPUP | WS_VISIBLE,
                         0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), 
                         nullptr, nullptr, wc.hInstance, nullptr);

    // Store global window handle for the keyboard hook
    g_overlayHwnd = m_hwnd;

    // Store pointer to this Overlay instance in window user data
    SetWindowLongPtr(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
                        
    // Set window to be transparent
    MARGINS margins = { -1 };
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);

    // Set window to topmost and click-through
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowLong(m_hwnd, GWL_EXSTYLE, GetWindowLong(m_hwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT | WS_EX_LAYERED);
    SetLayeredWindowAttributes(m_hwnd, RGB(0, 0, 0), 255, LWA_ALPHA);

    // Initialize Direct3D
    if (!CreateDeviceD3D())
    {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    // Show the window (but hide it initially)
    ShowWindow(m_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(m_hwnd);
    ShowWindow(m_hwnd, SW_HIDE); // Hide initially
    m_isVisible = false;

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    
    // Increase font size - add these lines
    ImFontConfig fontConfig;
    fontConfig.SizePixels = 16.0f; // Increase font size (default is usually around 13)
    io.Fonts->AddFontDefault(&fontConfig);

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    
    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(m_pd3dDevice, m_pd3dDeviceContext);

    // Set up keyboard hook
    g_keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
    if (!g_keyboardHook)
    {
        MessageBoxW(NULL, L"Failed to set keyboard hook", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }

    // Load settings if available
    LoadSettings();

    m_isRunning = true;
    return true;
}

void Overlay::Run()
{
    MSG msg;
    ZeroMemory(&msg, sizeof(msg));

    while (m_isRunning && msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        // Only render if visible
        if (m_isVisible)
        {
            // Start the Dear ImGui frame
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            // Render the overlay elements
            RenderOverlay();

            // Rendering
            ImGui::Render();
            const float clear_color_with_alpha[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            m_pd3dDeviceContext->OMSetRenderTargets(1, &m_mainRenderTargetView, NULL);
            m_pd3dDeviceContext->ClearRenderTargetView(m_mainRenderTargetView, clear_color_with_alpha);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            m_pSwapChain->Present(1, 0); // Present with vsync
        }
        else
        {
            // When not visible, sleep to reduce CPU usage
            Sleep(10);
        }
    }
}

void Overlay::Toggle()
{
    m_isVisible = !m_isVisible;
    ShowWindow(m_hwnd, m_isVisible ? SW_SHOW : SW_HIDE);
    
    // Update transparency settings based on visibility
    if (m_isVisible)
    {
        // When visible, disable the click-through behavior so overlay can capture clicks
        SetWindowLong(m_hwnd, GWL_EXSTYLE, 
            (GetWindowLong(m_hwnd, GWL_EXSTYLE) | WS_EX_LAYERED) & ~WS_EX_TRANSPARENT);
    }
    else
    {
        // When hidden, re-enable click-through
        SetWindowLong(m_hwnd, GWL_EXSTYLE, 
            GetWindowLong(m_hwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT | WS_EX_LAYERED);
    }
}

void Overlay::RenderOverlay()
{
    ImGuiIO& io = ImGui::GetIO();
    
    // Store the mouse position at the start of the frame
    static ImVec2 startDragPos;
    static bool dragging = false;
    
    // First: Draw the system info window
    ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    
    ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    
    // Set a minimum window width
    ImGui::SetNextWindowSizeConstraints(ImVec2(350, 0), ImVec2(FLT_MAX, FLT_MAX));
    
    // Enable dragging with left mouse button while holding the Control key
    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && io.MouseDown[0] && !dragging)
    {
        dragging = true;
        startDragPos = io.MousePos;
    }
    
    if (dragging && io.MouseDown[0])
    {
        ImVec2 currentPos = ImGui::GetWindowPos();
        ImVec2 newPos = ImVec2(currentPos.x + io.MouseDelta.x, currentPos.y + io.MouseDelta.y);
        ImGui::SetNextWindowPos(newPos);
    }
    else if (!io.MouseDown[0])
    {
        dragging = false;
    }
    
    // Make the window title reflect that it's draggable with Ctrl
    ImGui::Begin("System Info Overlay (Ctrl+Drag to move)", nullptr, 
        ImGuiWindowFlags_NoDecoration | 
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing);
    
    // Add a title header with no special drag functionality
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "System Information");
    ImGui::SameLine(ImGui::GetWindowWidth() - 80);
    
    // Settings button
    if (ImGui::Button("Settings"))
    {
        m_showSettings = !m_showSettings;
    }

    ImGui::Separator();
    // Add Audio Window toggle button
    if (ImGui::Button("Audio"))
    {
        ToggleAudioWindow();
    }
    
    // Add Network Window toggle button next to Audio button
    ImGui::SameLine();
    if (ImGui::Button("Network"))
    {
        ToggleNetworkWindow();
    }
    
    ImGui::Separator();
    
    // Store the window position and size to detect clicks
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();
    
    // Show settings panel if enabled
    if (m_showSettings)
    {
        RenderSettingsPanel();
        ImGui::Separator();
    }
    
    // More compact layout for system info sections
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));  // Slightly increased spacing
    
    if (m_settings.showCpuInfo)
    {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "CPU INFO");
        
        // Put CPU usage on its own line
        ImGui::Text("Usage: %d%%", GetCPUUsage());
        
        // CPU temperature on another line
        if (m_settings.showCpuTemperature)
        {
            ImGui::Text("Temperature: %d°C", GetCPUTemperature());
        }
        
        // Add a small spacing after the CPU section
        ImGui::Spacing();
    }
    
    if (m_settings.showMemoryInfo)
    {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "MEMORY");
        MEMORYSTATUSEX memInfo = GetMemoryInfo();
        float usedMemoryGB = (float)(memInfo.ullTotalPhys - memInfo.ullAvailPhys) / (1024 * 1024 * 1024);
        float totalMemoryGB = (float)memInfo.ullTotalPhys / (1024 * 1024 * 1024);
        float memoryUsagePercent = (usedMemoryGB / totalMemoryGB) * 100.0f;
        
        ImGui::Text("Usage:");
        ImGui::SameLine(120); // Increase from 100 to 120
        ImGui::Text("%.1f/%.1f GB", usedMemoryGB, totalMemoryGB);
        
        // Small, centered progress bar
        float barWidth = 200.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - barWidth) * 0.5f);
        ImGui::ProgressBar(memoryUsagePercent / 100.0f, ImVec2(barWidth, 8), "");
    }
    
    if (m_settings.showBatteryInfo)
    {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "BATTERY");
        
        int batteryPercent;
        bool isCharging;
        int remainingMinutes;
        
        if (GetBatteryStatus(batteryPercent, isCharging, remainingMinutes))
        {
            // Battery percentage and status
            ImGui::Text("Level:");
            ImGui::SameLine(100);
            
            // Choose color based on battery level
            ImVec4 batteryColor;
            if (isCharging)
                batteryColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); // Green when charging
            else if (batteryPercent < 20)
                batteryColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f); // Red when low
            else if (batteryPercent < 50)
                batteryColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // Yellow when medium
            else
                batteryColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); // Green when high
                
            ImGui::TextColored(batteryColor, "%d%%  %s", 
                batteryPercent, 
                isCharging ? "🔌" : "🔋");
            
            // Remaining time (only show when discharging and value is known)
            if (!isCharging && remainingMinutes > 0)
            {
                int hours = remainingMinutes / 60;
                int mins = remainingMinutes % 60;
                ImGui::Text("Remaining:");
                ImGui::SameLine(100);
                ImGui::Text("%dh %dm", hours, mins);
            }
            else if (isCharging)
            {
                ImGui::Text("Status:");
                ImGui::SameLine(100);
                ImGui::Text("Charging");
            }
            
            // Battery progress bar
            float barWidth = 200.0f;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - barWidth) * 0.5f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, batteryColor);
            ImGui::ProgressBar(batteryPercent / 100.0f, ImVec2(barWidth, 8), "");
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::Text("No battery detected");
        }
        
        ImGui::Spacing();
    }
    
    ImGui::PopStyleVar();  // Restore item spacing
    
    

    ImGui::End();

    // Render audio window (this will set m_mouseInsideAudioWindow)
    if (m_showAudioWindow)
    {
        RenderAudioWindow();
    }

    if (m_showNetworkWindow)
    {
        RenderNetworkWindow();
    }
    

    // Draw background
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y));
    ImGui::SetNextWindowBgAlpha(0.5f);
    
    ImGui::Begin("##Overlay_Background", nullptr, 
        ImGuiWindowFlags_NoDecoration | 
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoInputs);
    
    ImGui::End();

    // Store if we're currently interacting with dropdown menus or popups
    bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    bool comboActive = ImGui::IsPopupOpen("##AudioDevice");
    
    // Use the windowPos and windowSize variables declared earlier
    // (No need to redeclare them here)
    
    // Check if mouse click is inside the system info window
    bool mouseInsideWindow = (io.MousePos.x >= windowPos.x && 
                             io.MousePos.x <= windowPos.x + windowSize.x &&
                             io.MousePos.y >= windowPos.y &&
                             io.MousePos.y <= windowPos.y + windowSize.y);
    
    // Only toggle OFF if click is outside all windows AND not in Flow Launcher
    // AND no popup/combo is active
    if (ImGui::IsMouseClicked(0) && !mouseInsideWindow && !m_mouseInsideAudioWindow && 
        !m_mouseInsideNetworkWindow && !IsClickedInFlowLauncher() && !popupOpen && !comboActive)
    {
        // Add a small delay to prevent immediate toggling
        static DWORD lastClickTime = 0;
        DWORD currentTime = GetTickCount();
        
        if (currentTime - lastClickTime > 200) { // 200ms debounce
            Toggle();
            lastClickTime = currentTime;
        }
    }
}

void Overlay::RenderSettingsPanel()
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.15f, 0.9f));
    
    // Use a more compact size and style for the settings panel
    ImGui::BeginChild("SettingsPanel", ImVec2(ImGui::GetWindowWidth() * 0.9f, 140), true);
    
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "DISPLAY SETTINGS");
    ImGui::Separator();
    
    // Use columns to make the settings more compact
    ImGui::Columns(2, "SettingsColumns", false);
    
    // Left column
    ImGui::Checkbox("CPU Info", &m_settings.showCpuInfo);
    ImGui::Checkbox("Memory Info", &m_settings.showMemoryInfo);
    ImGui::Checkbox("Battery Info", &m_settings.showBatteryInfo);  // Add this line
    
    // Right column
    ImGui::NextColumn();
    ImGui::Checkbox("CPU Temperature", &m_settings.showCpuTemperature);
    ImGui::Checkbox("Network Info", &m_settings.showNetworkInfo);
    
    // Add audio controls checkbox (either column works)
    ImGui::Checkbox("Audio Controls", &m_settings.showAudioControls);
    
    // Reset columns
    ImGui::Columns(1);
    ImGui::Separator();
    
    // Save/Cancel buttons - put them on the same line
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 240) * 0.5f);  // Center the buttons
    if (ImGui::Button("Save", ImVec2(100, 0)))
    {
        SaveSettings();
        m_settings.saveToFile = true;
        m_showSettings = false;
    }
    
    ImGui::SameLine();
    
    if (ImGui::Button("Cancel", ImVec2(100, 0)))
    {
        if (m_settings.saveToFile)
        {
            LoadSettings();
        }
        else
        {
            memset(&m_settings, 0, sizeof(m_settings));
        }
        m_showSettings = false;
    }
    
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// Add this function to implement the audio settings panel

void Overlay::RenderAudioSettingsPanel()
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.15f, 0.9f));
    
    // Create a child window for settings
    ImGui::BeginChild("AudioSettingsPanel", ImVec2(ImGui::GetWindowWidth() * 0.9f, 140), true);
    
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "AUDIO SETTINGS");
    ImGui::Separator();
    
    // Audio-specific settings
    ImGui::Checkbox("Show Volume Percentage", &m_settings.audioSettings.showVolumePercentage);
    ImGui::Checkbox("Show Device Selector", &m_settings.audioSettings.showDeviceSelector);
    ImGui::Checkbox("Always On Top", &m_settings.audioSettings.alwaysOnTop);
    ImGui::Checkbox("Save Window Position", &m_settings.audioSettings.savePosition);
    
    ImGui::Separator();
    
    // Button to apply settings
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 120) * 0.5f);  // Center the button
    if (ImGui::Button("Apply", ImVec2(120, 30)))
    {
        // Apply settings (like always-on-top)
        if (m_settings.audioSettings.alwaysOnTop)
        {
            // In a real implementation, you'd set the audio window to be topmost
            // This is a placeholder since we're using ImGui windows
        }
        
        SaveSettings(); // Save to persistent storage
        m_showAudioSettings = false; // Close settings panel
    }
    
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void Overlay::Cleanup()
{
    m_isRunning = false;
    
    // Cleanup PDH resources
    if (m_pdh_initialized) {
        PdhCloseQuery(m_cpuQuery);
        m_pdh_initialized = false;
    }
    
    // Unhook keyboard hook
    if (g_keyboardHook)
    {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = NULL;
    }
    
    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(m_hwnd);
    UnregisterClassW(L"WindowsInfoOverlay", GetModuleHandle(NULL));
}

bool Overlay::CreateDeviceD3D()
{
    // Setup swap chain
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &m_pSwapChain, &m_pd3dDevice, &featureLevel, &m_pd3dDeviceContext);
    
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void Overlay::CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    m_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &m_mainRenderTargetView);
    pBackBuffer->Release();
}

void Overlay::CleanupRenderTarget()
{
    if (m_mainRenderTargetView) { m_mainRenderTargetView->Release(); m_mainRenderTargetView = nullptr; }
}

void Overlay::CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (m_pSwapChain) { m_pSwapChain->Release(); m_pSwapChain = nullptr; }
    if (m_pd3dDeviceContext) { m_pd3dDeviceContext->Release(); m_pd3dDeviceContext = nullptr; }
    if (m_pd3dDevice) { m_pd3dDevice->Release(); m_pd3dDevice = nullptr; }
}

// Initialize the PDH query for CPU usage
void Overlay::InitializeCpuCounter()
{
    PDH_STATUS status = PdhOpenQuery(NULL, 0, &m_cpuQuery);
    if (status != ERROR_SUCCESS) return;

    // Add the counter
    status = PdhAddEnglishCounter(m_cpuQuery, "\\Processor(_Total)\\% Processor Time", 0, &m_cpuTotal);
    if (status != ERROR_SUCCESS) {
        PdhCloseQuery(m_cpuQuery);
        return;
    }

    // First call to collect, subsequent calls will use this as baseline
    status = PdhCollectQueryData(m_cpuQuery);
    if (status != ERROR_SUCCESS) {
        PdhCloseQuery(m_cpuQuery);
        return;
    }

    m_pdh_initialized = true;
}

// Implement real CPU usage monitoring
int Overlay::GetCPUUsage()
{
    if (!m_pdh_initialized) return 0;

    PDH_FMT_COUNTERVALUE counterVal;
    
    // Collect current value
    PDH_STATUS status = PdhCollectQueryData(m_cpuQuery);
    if (status != ERROR_SUCCESS) return 0;
    
    // Format the performance data
    status = PdhGetFormattedCounterValue(m_cpuTotal, PDH_FMT_LONG, NULL, &counterVal);
    if (status != ERROR_SUCCESS) return 0;
    
    int currentCpuUsage = static_cast<int>(counterVal.longValue);
    
    // Add to history
    m_cpuUsageHistory.push_back(currentCpuUsage);
    
    // Keep history to fixed size
    if (m_cpuUsageHistory.size() > CPU_HISTORY_SIZE) {
        m_cpuUsageHistory.erase(m_cpuUsageHistory.begin());
    }
    
    // Calculate average
    int totalUsage = 0;
    for (const auto& usage : m_cpuUsageHistory) {
        totalUsage += usage;
    }
    
    m_smoothedCpuUsage = m_cpuUsageHistory.size() > 0 ? 
        totalUsage / static_cast<int>(m_cpuUsageHistory.size()) : currentCpuUsage;
    
    return m_smoothedCpuUsage;
}

// Implement real CPU temperature monitoring using WMI
int Overlay::GetCPUTemperature()
{
    int temperature = 0;
    
    // Initialize COM
    HRESULT hr = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hr)) return 0;
    
    // Set general COM security levels
    hr = CoInitializeSecurity(
        NULL,
        -1,                          // COM authentication
        NULL,                        // Authentication services
        NULL,                        // Reserved
        RPC_C_AUTHN_LEVEL_DEFAULT,   // Default authentication 
        RPC_C_IMP_LEVEL_IMPERSONATE, // Default Impersonation  
        NULL,                        // Authentication info
        EOAC_NONE,                   // Additional capabilities 
        NULL                         // Reserved
    );

    if (FAILED(hr)) {
        CoUninitialize();
        return 0;
    }
    
    // Obtain the initial locator to WMI
    IWbemLocator *pLoc = NULL;
    hr = CoCreateInstance(
        CLSID_WbemLocator,             
        0, 
        CLSCTX_INPROC_SERVER, 
        IID_IWbemLocator, (LPVOID *) &pLoc);
    
    if (FAILED(hr)) {
        CoUninitialize();
        return 0;
    }
    
    // Connect to WMI through the IWbemLocator::ConnectServer method
    IWbemServices *pSvc = NULL;
    hr = pLoc->ConnectServer(
        _bstr_t(L"ROOT\\WMI"),      // Object path of WMI namespace
        NULL,                    // User name. NULL = current user
        NULL,                    // User password. NULL = current
        0,                       // Locale. NULL indicates current
        0,                       // Security flags - use 0 instead of NULL
        0,                       // Authority (e.g. Kerberos)
        0,                       // Context object 
        &pSvc                    // pointer to IWbemServices proxy
    );
    
    if (FAILED(hr)) {
        pLoc->Release();     
        CoUninitialize();
        return 0;
    }
    
    // Set security levels on the proxy
    hr = CoSetProxyBlanket(
        pSvc,                        // Indicates the proxy to set
        RPC_C_AUTHN_WINNT,           // RPC_C_AUTHN_xxx
        RPC_C_AUTHZ_NONE,            // RPC_C_AUTHZ_xxx
        NULL,                        // Server principal name 
        RPC_C_AUTHN_LEVEL_CALL,      // RPC_C_AUTHN_LEVEL_xxx 
        RPC_C_IMP_LEVEL_IMPERSONATE, // RPC_C_IMP_LEVEL_xxx
        NULL,                        // client identity
        EOAC_NONE                    // proxy capabilities 
    );

    if (FAILED(hr)) {
        pSvc->Release();
        pLoc->Release();     
        CoUninitialize();
        return 0;
    }
    
    // Use the IWbemServices pointer to make requests of WMI
    IEnumWbemClassObject* pEnumerator = NULL;
    BSTR wqlBstr = SysAllocString(L"WQL");
    BSTR queryBstr = SysAllocString(L"SELECT * FROM MSAcpi_ThermalZoneTemperature");
    hr = pSvc->ExecQuery(
        wqlBstr, 
        queryBstr,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, 
        NULL,
        &pEnumerator);
    SysFreeString(wqlBstr);
    SysFreeString(queryBstr);
    
    if (FAILED(hr)) {
        pSvc->Release();
        pLoc->Release();
        CoUninitialize();
        return 65; // Fallback value if WMI query fails
    }
    
    // Get the data from the WMI query
    IWbemClassObject *pclsObj = NULL;
    ULONG uReturn = 0;
   
    while (pEnumerator) {
        HRESULT hr = pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn);

        if (uReturn == 0) break;

        VARIANT vtProp;
        
        // First try to get "CurrentTemperature" property
        hr = pclsObj->Get(L"CurrentTemperature", 0, &vtProp, 0, 0);
        if (SUCCEEDED(hr)) {
            // WMI returns the temperature in tenths of Kelvin
            // Convert to Celsius: (K - 273.15)
            double kelvin = vtProp.intVal / 10.0;
            temperature = static_cast<int>(kelvin - 273.15);
            VariantClear(&vtProp);
            break;
        }
        
        // If that fails, try "CurrentReading" property
        hr = pclsObj->Get(L"CurrentReading", 0, &vtProp, 0, 0);
        if (SUCCEEDED(hr)) {
            temperature = vtProp.intVal;
            VariantClear(&vtProp);
            break;
        }
        
        pclsObj->Release();
    }
    
    // Clean up
    pEnumerator->Release();
    pSvc->Release();
    pLoc->Release();
    CoUninitialize();
    
    return temperature > 0 ? temperature : 65; // Return sensible default if we failed
}

MEMORYSTATUSEX Overlay::GetMemoryInfo()
{
    MEMORYSTATUSEX memInfo = GetMemoryStatusEx(); // Use the helper function
    GlobalMemoryStatusEx(&memInfo);
    return memInfo;
}

bool Overlay::IsClickedInFlowLauncher()
{
    // First approach: Find Flow Launcher by window class or title
    HWND flowLauncherHwnd = FindWindowW(NULL, L"Flow.Launcher");
    if (!flowLauncherHwnd)
    {
        // Try alternative names if the above doesn't find it
        flowLauncherHwnd = FindWindowW(NULL, L"Flow Launcher");
    }
    
    if (flowLauncherHwnd)
    {
        // Get Flow Launcher's position and size
        RECT rect;
        if (GetWindowRect(flowLauncherHwnd, &rect))
        {
            // Get current mouse position
            POINT mousePos;
            GetCursorPos(&mousePos);
            
            // Check if mouse is inside Flow Launcher's rectangle
            if (mousePos.x >= rect.left && mousePos.x <= rect.right &&
                mousePos.y >= rect.top && mousePos.y <= rect.bottom)
            {
                return true;
            }
        }
    }
    
    // Second approach: Define a fixed area where Flow Launcher appears
    // This is a fallback if we couldn't find the window
    ImGuiIO& io = ImGui::GetIO();
    float screenWidth = io.DisplaySize.x;
    float screenHeight = io.DisplaySize.y;
    
    // Assume Flow Launcher appears in the center top portion of screen
    // Adjust these values based on your Flow Launcher's typical position
    float flowLeft = screenWidth * 0.2f;
    float flowRight = screenWidth * 0.8f;
    float flowTop = screenHeight * 0.1f;
    float flowBottom = screenHeight * 0.3f;
    
    // Check if mouse is inside estimated Flow Launcher area
    ImVec2 mousePos = io.MousePos;
    if (mousePos.x >= flowLeft && mousePos.x <= flowRight &&
        mousePos.y >= flowTop && mousePos.y <= flowBottom)
    {
        return true;
    }
    
    return false;
}

void Overlay::SaveSettings()
{
    std::string filePath = GetSettingsFilePath();
    FILE* file = fopen(filePath.c_str(), "wb");
    if (file)
    {
        fwrite(&m_settings, sizeof(OverlaySettings), 1, file);
        fclose(file);
    }
}

void Overlay::LoadSettings()
{
    std::string filePath = GetSettingsFilePath();
    FILE* file = fopen(filePath.c_str(), "rb");
    if (file)
    {
        fread(&m_settings, sizeof(OverlaySettings), 1, file);
        fclose(file);
    }
    else
    {
        // No settings file exists yet, use defaults
        memset(&m_settings, 0, sizeof(m_settings));
    }
}

std::string Overlay::GetSettingsFilePath()
{
    char appDataPath[MAX_PATH];
    SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appDataPath);
    std::string path = std::string(appDataPath) + "\\WindowsInfoOverlay";
    
    // Create directory if it doesn't exist
    if (CreateDirectoryA(path.c_str(), NULL) || 
        GetLastError() == ERROR_ALREADY_EXISTS)
    {
        return path + "\\settings.dat";
    }
    
    // Fallback to current directory if appdata isn't accessible
    return "settings.dat";
}

// Add these new functions

void Overlay::ToggleAudioWindow()
{
    m_showAudioWindow = !m_showAudioWindow;
    
    // If showing for the first time, position it near the system info window
    if (m_showAudioWindow && m_isVisible)
    {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 mainWinPos = ImGui::GetWindowPos();
        m_audioWindowPos = ImVec2(mainWinPos.x + 50, mainWinPos.y + 50);
        
        // Make sure we have updated device information
        m_audioManager.RefreshDevices();
    }
}

void Overlay::ToggleNetworkWindow()
{
    m_showNetworkWindow = !m_showNetworkWindow;
    
    // If showing for the first time, position it near the system info window
    if (m_showNetworkWindow && m_isVisible)
    {
        ImGuiIO& io = ImGui::GetIO();
        
        // Position it differently than the audio window to avoid overlap
        // Use a different offset to make it appear in a different position
        ImVec2 mainWinPos = ImGui::GetWindowPos();
        m_networkWindowPos = ImVec2(mainWinPos.x + 100, mainWinPos.y + 100);
    }
}

void Overlay::RenderAudioWindow()
{
    if (!m_showAudioWindow) return;
    
    ImGuiIO& io = ImGui::GetIO();
    
    // Set position for the audio window
    ImGui::SetNextWindowPos(m_audioWindowPos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(350, 250), ImVec2(500, 400));
    
    // Variables for dragging
    static bool audioDragging = false;
    
    // Enable dragging with Ctrl key
    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && io.MouseDown[0] && !audioDragging)
    {
        audioDragging = true;
    }
    
    if (audioDragging && io.MouseDown[0])
    {
        m_audioWindowPos.x += io.MouseDelta.x;
        m_audioWindowPos.y += io.MouseDelta.y;
        ImGui::SetNextWindowPos(m_audioWindowPos);
    }
    else if (!io.MouseDown[0])
    {
        audioDragging = false;
    }
    
    // Begin audio window (no "close" option in &m_showAudioWindow)
    ImGui::Begin("Audio Controls", nullptr, 
        ImGuiWindowFlags_NoDecoration | 
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing);
    
    // Store window position for next time
    m_audioWindowPos = ImGui::GetWindowPos();
    ImVec2 audioWindowSize = ImGui::GetWindowSize();
    
    // Add header with settings and close buttons
    ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "Audio Control Panel");
    
    // Settings button
    ImGui::SameLine(ImGui::GetWindowWidth() - 60);
    if (ImGui::Button("⚙"))
    {
        m_showAudioSettings = !m_showAudioSettings;
    }
    
    // Close button
    ImGui::SameLine(ImGui::GetWindowWidth() - 25);
    if (ImGui::Button("X"))
    {
        m_showAudioWindow = false;
        ImGui::End();
        return;
    }
    
    ImGui::Separator();
    
    // Show audio settings panel if enabled
    if (m_showAudioSettings)
    {
        RenderAudioSettingsPanel();
        ImGui::Separator();
    }
    
    // Volume controls section
    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
    
    // Get current volume and mute state
    float volume = m_audioManager.GetMasterVolume();
    bool muted = m_audioManager.IsMasterMuted();
    
    // Volume display (optional based on settings)
    if (m_settings.audioSettings.showVolumePercentage)
    {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Master Volume: %d%%", static_cast<int>(volume * 100));
    }
    
    // Mute toggle button with icon and colorful styling
    ImGui::PushStyleColor(ImGuiCol_Button, muted ? ImVec4(0.6f, 0.1f, 0.1f, 1.0f) : ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, muted ? ImVec4(0.8f, 0.2f, 0.2f, 1.0f) : ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
    
    if (ImGui::Button(muted ? "🔇 MUTED" : "🔊 UNMUTED", ImVec2(ImGui::GetWindowWidth() * 0.9f, 40)))
    {
        m_audioManager.SetMasterMuted(!muted);
    }
    ImGui::PopStyleColor(2);
    
    ImGui::Spacing();
    
    // Volume slider with custom styling
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.3f, 0.7f, 0.9f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.5f, 0.8f, 1.0f, 1.0f));
    
    // Larger volume slider
    ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * 0.9f);
    if (ImGui::SliderFloat("##Volume", &volume, 0.0f, 1.0f, ""))
    {
        m_audioManager.SetMasterVolume(volume);
    }
    ImGui::PopStyleColor(3);
    
    ImGui::Spacing();
    
    // Show device selector only if enabled in settings
    if (m_settings.audioSettings.showDeviceSelector)
    {
        ImGui::Separator();
        
        // Audio device selection
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.7f, 1.0f), "Output Device:");
        ImGui::Spacing();
        
        // Custom style for the combo box
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.2f, 0.2f, 0.2f, 0.98f));
        
        // Get the devices list and selected device from the manager
        const auto& devices = m_audioManager.GetDevices();
        int selectedDevice = m_audioManager.GetSelectedDeviceIndex();
        
        // Device dropdown with description
        ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * 0.9f);
        if (ImGui::BeginCombo("##AudioDevice", devices[selectedDevice].first.c_str()))
        {
            for (int i = 0; i < devices.size(); i++)
            {
                bool is_selected = (selectedDevice == i);
                if (ImGui::Selectable(devices[i].first.c_str(), is_selected))
                {
                    m_audioManager.SetDevice(i);
                }
                if (is_selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleColor(2);
        
        ImGui::Spacing();
        
        // Refresh button with better styling
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 120) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.6f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.5f, 0.7f, 1.0f));
        if (ImGui::Button("↻ Refresh Devices", ImVec2(120, 30)))
        {
            m_audioManager.RefreshDevices();
        }
        ImGui::PopStyleColor(2);
    }
    
    ImGui::PopFont();
    ImGui::End();
    
    // Check if mouse is inside audio window - we'll use this in RenderOverlay
    bool mouseInsideAudio = (io.MousePos.x >= m_audioWindowPos.x && 
                         io.MousePos.x <= m_audioWindowPos.x + audioWindowSize.x &&
                         io.MousePos.y >= m_audioWindowPos.y &&
                         io.MousePos.y <= m_audioWindowPos.y + audioWindowSize.y);
                         
    // Store this value to be accessed from RenderOverlay
    m_mouseInsideAudioWindow = mouseInsideAudio;
}

void Overlay::RenderNetworkWindow()
{
    if (!m_showNetworkWindow) return;
    
    ImGuiIO& io = ImGui::GetIO();
    
    // Set position for the network window
    ImGui::SetNextWindowPos(m_networkWindowPos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(350, 250), ImVec2(500, 400));
    
    // Variables for dragging
    static bool networkDragging = false;
    
    // Enable dragging with Ctrl key
    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && io.MouseDown[0] && !networkDragging)
    {
        networkDragging = true;
    }
    
    if (networkDragging && io.MouseDown[0])
    {
        m_networkWindowPos.x += io.MouseDelta.x;
        m_networkWindowPos.y += io.MouseDelta.y;
        ImGui::SetNextWindowPos(m_networkWindowPos);
    }
    else if (!io.MouseDown[0])
    {
        networkDragging = false;
    }
    
    // Begin network window
    ImGui::Begin("Network Controls", nullptr, 
        ImGuiWindowFlags_NoDecoration | 
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing);
    
    // Store window position for next time
    m_networkWindowPos = ImGui::GetWindowPos();
    ImVec2 networkWindowSize = ImGui::GetWindowSize();
    
    // Add header with settings and close buttons
    ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "Network Control Panel");
    
    // Settings button
    ImGui::SameLine(ImGui::GetWindowWidth() - 60);
    if (ImGui::Button("⚙"))
    {
        m_showNetworkSettings = !m_showNetworkSettings;
    }
    
    // Close button
    ImGui::SameLine(ImGui::GetWindowWidth() - 25);
    if (ImGui::Button("X"))
    {
        m_showNetworkWindow = false;
        ImGui::End();
        return;
    }
    
    ImGui::Separator();
    
    // Show network settings panel if enabled
    if (m_showNetworkSettings)
    {
        RenderNetworkSettingsPanel();
        ImGui::Separator();
    }
    
    // Network controls section
    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
    
    // WiFi On/Off button with status
    bool wifiEnabled = m_networkManager.IsWifiEnabled();
    ImGui::PushStyleColor(ImGuiCol_Button, wifiEnabled ? ImVec4(0.1f, 0.6f, 0.1f, 1.0f) : ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, wifiEnabled ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f) : ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    
    if (ImGui::Button(wifiEnabled ? "WiFi: ON" : "WiFi: OFF", ImVec2(ImGui::GetWindowWidth() * 0.9f, 40)))
    {
        // Toggle WiFi state using the manager
        m_networkManager.ToggleWifi(!wifiEnabled);
    }
    ImGui::PopStyleColor(2);
    
    ImGui::Spacing();
    
    // Current connection information
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.7f, 1.0f), "Current Connection:");
    
    ImGui::BeginChild("ConnectionInfo", ImVec2(ImGui::GetWindowWidth() * 0.9f, 70), true);
    
    ImGui::Text("Network: %s", m_networkManager.GetCurrentNetworkName().c_str());
    ImGui::Separator();
    
    // Network speeds using the manager
    ImGui::Text("Download: %.2f MB/s", m_networkManager.GetDownloadSpeed());
    ImGui::Text("Upload: %.2f MB/s", m_networkManager.GetUploadSpeed());
    
    ImGui::EndChild();
    
    ImGui::Spacing();
    
    // Available networks section (if enabled in settings)
    if (m_settings.networkSettings.showNetworkDetails)
    {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.7f, 1.0f), "Available Networks:");
        
        // Button to scan for networks
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.6f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.5f, 0.7f, 1.0f));
        
        if (m_networkManager.IsScanning())
        {
            ImGui::BeginDisabled(true);
            ImGui::Button("Scanning...", ImVec2(120, 30));
            ImGui::EndDisabled();
        }
        else
        {
            if (ImGui::Button("Scan Networks", ImVec2(120, 30)))
            {
                m_networkManager.ScanNetworks();
            }
        }
        ImGui::PopStyleColor(2);
        
        // List of available networks
        ImGui::BeginChild("AvailableNetworks", ImVec2(ImGui::GetWindowWidth() * 0.9f, 120), true);
        
        const auto& availableNetworks = m_networkManager.GetAvailableNetworks();
        if (availableNetworks.empty())
        {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No networks found or scan not performed");
        }
        else
        {
            std::string currentNetwork = m_networkManager.GetCurrentNetworkName();
            for (const auto& network : availableNetworks)
            {
                bool isCurrentNetwork = (network.first == currentNetwork);
                
                if (isCurrentNetwork)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
                }
                
                if (ImGui::Selectable(network.first.c_str(), isCurrentNetwork))
                {
                    // Connect to network logic would go here
                    // This would require additional implementation
                    // You could invoke Windows connection dialog or a custom one
                }
                
                if (isCurrentNetwork)
                {
                    ImGui::PopStyleColor();
                }
            }
        }
        
        ImGui::EndChild();
    }
    
    ImGui::PopFont();
    ImGui::End();
    
    // Check if mouse is inside network window - we'll use this in RenderOverlay
    bool mouseInsideNetwork = (io.MousePos.x >= m_networkWindowPos.x && 
                         io.MousePos.x <= m_networkWindowPos.x + networkWindowSize.x &&
                         io.MousePos.y >= m_networkWindowPos.y &&
                         io.MousePos.y <= m_networkWindowPos.y + networkWindowSize.y);
                         
    // Store this value to be accessed from RenderOverlay
    m_mouseInsideNetworkWindow = mouseInsideNetwork;
}

void Overlay::RenderNetworkSettingsPanel()
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.15f, 0.9f));
    
    // Create a child window for settings
    ImGui::BeginChild("NetworkSettingsPanel", ImVec2(ImGui::GetWindowWidth() * 0.9f, 110), true);
    
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "NETWORK SETTINGS");
    ImGui::Separator();
    
    // Network-specific settings
    ImGui::Checkbox("Show Network Details", &m_settings.networkSettings.showNetworkDetails);
    ImGui::Checkbox("Always On Top", &m_settings.networkSettings.alwaysOnTop);
    ImGui::Checkbox("Save Window Position", &m_settings.networkSettings.savePosition);
    
    ImGui::Separator();
    
    // Button to apply settings
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 120) * 0.5f);  // Center the button
    if (ImGui::Button("Apply", ImVec2(120, 30)))
    {
        // Apply settings (like always-on-top)
        if (m_settings.networkSettings.alwaysOnTop)
        {
            // In a real implementation, you'd set the network window to be topmost
            // This is a placeholder since we're using ImGui windows
        }
        
        SaveSettings(); // Save to persistent storage
        m_showNetworkSettings = false; // Close settings panel
    }
    
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// Add this function to get battery status
bool Overlay::GetBatteryStatus(int& batteryPercent, bool& isCharging, int& remainingMinutes)
{
    SYSTEM_POWER_STATUS powerStatus;
    if (GetSystemPowerStatus(&powerStatus))
    {
        // Get battery percentage
        if (powerStatus.BatteryLifePercent <= 100)
            batteryPercent = powerStatus.BatteryLifePercent;
        else
            batteryPercent = 0; // Unknown

        // Check if it's charging
        isCharging = (powerStatus.ACLineStatus == 1);
        
        // Get remaining minutes
        if (powerStatus.BatteryLifeTime != BATTERY_LIFE_UNKNOWN)
            remainingMinutes = powerStatus.BatteryLifeTime / 60; // Convert from seconds to minutes
        else
            remainingMinutes = -1; // Unknown
        
        return true;
    }
    
    return false;
}