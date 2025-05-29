#include "Overlay.h"
#include <dwmapi.h>
#include <iostream>
#include <iphlpapi.h>
#include <vector>
#include <pdh.h>
#include <wbemidl.h>
#include <comdef.h>
#include <queue>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wbemuuid.lib")

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
                overlay->Toggle();
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

Overlay::Overlay() : m_hwnd(nullptr), m_pd3dDevice(nullptr), m_pd3dDeviceContext(nullptr), 
    m_pSwapChain(nullptr), m_mainRenderTargetView(nullptr), m_isRunning(false), m_isVisible(false),
    m_lastInBytes(0), m_lastOutBytes(0), m_lastTickCount(0), m_downloadSpeed(0.0f), m_uploadSpeed(0.0f),
    m_cpuQuery(NULL), m_cpuTotal(NULL), m_pdh_initialized(false), m_smoothedCpuUsage(0)
{
    // Initialize PDH
    InitializeCpuCounter();
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
}

void Overlay::RenderOverlay()
{
    ImGuiIO& io = ImGui::GetIO();
    
    // FIRST: Draw the full-screen dimming background
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y));
    ImGui::SetNextWindowBgAlpha(0.5f);
    
    // Make the background window use ImGuiWindowFlags_NoInputs so clicks fall through it 
    ImGui::Begin("##Overlay_Background", nullptr, 
        ImGuiWindowFlags_NoDecoration | 
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoInputs | // This prevents the background from capturing clicks
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing);
    
    ImGui::End();
    
    // SECOND: Draw the system info window that receives all clicks
    ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    
    ImGui::Begin("System Info Overlay", nullptr, 
        ImGuiWindowFlags_NoDecoration | 
        ImGuiWindowFlags_AlwaysAutoResize | 
        ImGuiWindowFlags_NoSavedSettings | 
        ImGuiWindowFlags_NoFocusOnAppearing);

    // Store info window position and size for click detection
    ImVec2 infoWindowPos = ImGui::GetWindowPos();
    ImVec2 infoWindowSize = ImGui::GetWindowSize();
    
    // Instead of using collapsing headers, let's use sections with titles
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "CPU INFO");
    ImGui::Separator();
    ImGui::Text("CPU Usage: %d%%", GetCPUUsage());
    ImGui::Text("CPU Temperature: %d°C", GetCPUTemperature());
    ImGui::Spacing();
    
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "MEMORY INFO");
    ImGui::Separator();
    MEMORYSTATUSEX memInfo = GetMemoryInfo();
    float usedMemoryGB = (float)(memInfo.ullTotalPhys - memInfo.ullAvailPhys) / (1024 * 1024 * 1024);
    float totalMemoryGB = (float)memInfo.ullTotalPhys / (1024 * 1024 * 1024);
    float memoryUsagePercent = (usedMemoryGB / totalMemoryGB) * 100.0f;
    
    ImGui::Text("Memory Usage: %.1f GB / %.1f GB (%.1f%%)", 
              usedMemoryGB, totalMemoryGB, memoryUsagePercent);
    ImGui::ProgressBar(memoryUsagePercent / 100.0f, ImVec2(-1, 0), "");
    ImGui::Spacing();
    
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "NETWORK INFO");
    ImGui::Separator();
    ImGui::Text("Download Speed: %.2f MB/s", GetNetworkDownloadSpeed());
    ImGui::Text("Upload Speed: %.2f MB/s", GetNetworkUploadSpeed());
    
    ImGui::End();
    
    // THIRD: Create a transparent full-screen window to capture clicks outside the info window
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y));
    ImGui::SetNextWindowBgAlpha(0.0f); // Fully transparent
    
    ImGui::Begin("##ClickCatcher", nullptr, 
        ImGuiWindowFlags_NoDecoration | 
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBackground); // No background
    
    // Check for clicks outside the info window
    if (ImGui::IsMouseClicked(0))
    {
        // Get info window position and size from above
        ImVec2 mousePos = io.MousePos;
        
        // If click is outside info window and not in Flow Launcher, toggle off
        if (!(mousePos.x >= infoWindowPos.x && mousePos.x <= (infoWindowPos.x + infoWindowSize.x) &&
              mousePos.y >= infoWindowPos.y && mousePos.y <= (infoWindowPos.y + infoWindowSize.y)) 
            && !IsClickedInFlowLauncher())
        {
            Toggle();
        }
    }
    
    ImGui::End();
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
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    return memInfo;
}

float Overlay::GetNetworkDownloadSpeed()
{
    UpdateNetworkSpeeds();
    return m_downloadSpeed;
}

float Overlay::GetNetworkUploadSpeed()
{
    UpdateNetworkSpeeds();
    return m_uploadSpeed;
}

void Overlay::UpdateNetworkSpeeds()
{
    // Don't update too frequently
    DWORD currentTickCount = GetTickCount();
    if (m_lastTickCount != 0 && currentTickCount - m_lastTickCount < 1000)
    {
        return; // Only update once per second
    }
    
    // Get adapter info
    ULONG bufferSize = 0;
    GetIfTable(NULL, &bufferSize, FALSE);
    
    std::vector<BYTE> buffer(bufferSize);
    PMIB_IFTABLE ifTable = reinterpret_cast<PMIB_IFTABLE>(buffer.data());
    
    if (GetIfTable(ifTable, &bufferSize, FALSE) != NO_ERROR)
    {
        return;
    }
    
    // Sum up all bytes in/out across all network interfaces
    ULONG64 totalInBytes = 0;
    ULONG64 totalOutBytes = 0;
    
    for (DWORD i = 0; i < ifTable->dwNumEntries; i++)
    {
        MIB_IFROW& row = ifTable->table[i];
        
        // Skip interfaces with no traffic or loopback
        if (row.dwType != IF_TYPE_SOFTWARE_LOOPBACK &&
            (row.dwOperStatus == IF_OPER_STATUS_OPERATIONAL || 
             row.dwOperStatus == IF_OPER_STATUS_CONNECTED))
        {
            totalInBytes += row.dwInOctets;
            totalOutBytes += row.dwOutOctets;
        }
    }
    
    // Calculate speeds
    if (m_lastTickCount > 0)
    {
        float timeDelta = (currentTickCount - m_lastTickCount) / 1000.0f; // Convert to seconds
        
        if (timeDelta > 0)
        {
            // Calculate speeds in MB/s
            float inDelta = static_cast<float>(totalInBytes - m_lastInBytes);
            float outDelta = static_cast<float>(totalOutBytes - m_lastOutBytes);
            
            m_downloadSpeed = (inDelta / (1024 * 1024)) / timeDelta;
            m_uploadSpeed = (outDelta / (1024 * 1024)) / timeDelta;
            
            // Handle edge cases (like counter rollover)
            if (m_downloadSpeed < 0) m_downloadSpeed = 0;
            if (m_uploadSpeed < 0) m_uploadSpeed = 0;
        }
    }
    
    // Store current values for next calculation
    m_lastInBytes = totalInBytes;
    m_lastOutBytes = totalOutBytes;
    m_lastTickCount = currentTickCount;
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