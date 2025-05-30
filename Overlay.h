#pragma once

#include <windows.h>
#include <d3d11.h>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"
#include <vector>
#include <queue>
#include <string>
#include <pdh.h>

// Include our new manager classes
#include "AudioManager.h"
#include "NetworkManager.h"

// Define custom message for toggle
#define WM_TOGGLE_OVERLAY (WM_USER + 1)

#define CPU_HISTORY_SIZE 10

// Structure to hold overlay configuration settings
struct OverlaySettings 
{
    bool showCpuInfo = true;
    bool showCpuTemperature = true;
    bool showMemoryInfo = true;
    bool showNetworkInfo = true;
    bool showAudioControls = true;
    bool showBatteryInfo = true;
    bool saveToFile = false;
    AudioSettings audioSettings;
    NetworkSettings networkSettings;
};

// Helper function to get properly initialized MEMORYSTATUSEX
inline MEMORYSTATUSEX GetMemoryStatusEx()
{
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    return memInfo;
}

class Overlay
{
public:
    Overlay();
    ~Overlay();

    bool Initialize();
    void Run();
    void Toggle();
    void Cleanup();

    // Window and rendering
    friend LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    
    // These are public because they're accessed in WndProc
    HWND m_hwnd;
    ID3D11Device* m_pd3dDevice;
    ID3D11DeviceContext* m_pd3dDeviceContext;
    IDXGISwapChain* m_pSwapChain;
    ID3D11RenderTargetView* m_mainRenderTargetView;
    bool m_isVisible;

private:
    // Direct3D setup
    bool CreateDeviceD3D();
    void CleanupDeviceD3D();
    void CreateRenderTarget();
    void CleanupRenderTarget();

    // Rendering functions
    void RenderOverlay();
    void RenderSettingsPanel();
    void RenderAudioWindow();
    void RenderAudioSettingsPanel();
    void RenderNetworkWindow();
    void RenderNetworkSettingsPanel();

    // Toggle windows
    void ToggleAudioWindow();
    void ToggleNetworkWindow();

    // CPU monitoring
    void InitializeCpuCounter();
    int GetCPUUsage();
    int GetCPUTemperature();

    // Other system info
    MEMORYSTATUSEX GetMemoryInfo();
    bool GetBatteryStatus(int& batteryPercent, bool& isCharging, int& remainingMinutes);
    bool IsClickedInFlowLauncher();

    // Settings
    void SaveSettings();
    void LoadSettings();
    std::string GetSettingsFilePath();

    // Class members
    bool m_isRunning;
    bool m_showSettings;
    bool m_showAudioWindow;
    bool m_showAudioSettings;
    bool m_showNetworkWindow;
    bool m_showNetworkSettings;
    bool m_mouseInsideAudioWindow;
    bool m_mouseInsideNetworkWindow;
    ImVec2 m_audioWindowPos;
    ImVec2 m_networkWindowPos;
    OverlaySettings m_settings;

    // CPU monitoring variables
    PDH_HQUERY m_cpuQuery;
    PDH_HCOUNTER m_cpuTotal;
    bool m_pdh_initialized;
    std::vector<int> m_cpuUsageHistory;
    int m_smoothedCpuUsage;

    // Manager instances
    AudioManager m_audioManager;
    NetworkManager m_networkManager;

    ImFont* m_emojiFont = nullptr;
};