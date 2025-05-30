#pragma once

#include <Windows.h>
#include <d3d11.h>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <queue>
#include <pdh.h> // Include PDH header for performance data
#include <vector> // Include vector for CPU usage history
#include <string>
#include <shlobj.h>

// Forward declare these COM interfaces to avoid including the full headers in the .h file
// This is a cleaner approach that reduces compilation dependencies
struct IMMDeviceEnumerator;
struct IMMDevice;
struct IAudioEndpointVolume;

// Define a custom message for toggle
#define WM_TOGGLE_OVERLAY (WM_USER + 1)

// Structure to hold overlay configuration settings
struct AudioSettings {
    bool showVolumePercentage = true;
    bool showDeviceSelector = true;
    bool alwaysOnTop = false;
    bool savePosition = true;
};

// Network settings struct (add to OverlaySettings or as a separate struct)
struct NetworkSettings {
    bool showNetworkDetails = true;
    bool alwaysOnTop = false;
    bool savePosition = true;
};

// Add to OverlaySettings
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
    NetworkSettings networkSettings; // Add this line
};

class Overlay
{
public:
    Overlay();
    ~Overlay();
    
    bool Initialize();
    void Run();
    void Cleanup();
    void Toggle();

    friend LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void CleanupRenderTarget();
    void CreateRenderTarget();

    bool IsClickedInFlowLauncher();

private:
    // D3D and Window handling
    bool CreateDeviceD3D();
    void CleanupDeviceD3D();
    
    // Rendering
    void RenderOverlay();
    
    // System information functions
    int GetCPUUsage();
    int GetCPUTemperature();
    MEMORYSTATUSEX GetMemoryInfo();
    float GetNetworkDownloadSpeed();
    float GetNetworkUploadSpeed();
    
    // Window and D3D variables
    HWND m_hwnd;
    ID3D11Device* m_pd3dDevice;
    ID3D11DeviceContext* m_pd3dDeviceContext;
    IDXGISwapChain* m_pSwapChain;
    ID3D11RenderTargetView* m_mainRenderTargetView;
    bool m_isRunning;
    bool m_isVisible; // New variable to track visibility
    // Add this with your other private methods
    void UpdateNetworkSpeeds();
    void InitializeCpuCounter(); // Method to initialize CPU counter
    
    // Add these with your other private member variables
    ULONG64 m_lastInBytes;
    ULONG64 m_lastOutBytes;
    DWORD m_lastTickCount;
    float m_downloadSpeed;
    float m_uploadSpeed;

    // PDH variables for CPU usage
    PDH_HQUERY m_cpuQuery;
    PDH_HCOUNTER m_cpuTotal;
    bool m_pdh_initialized;
    int m_smoothedCpuUsage;

    std::vector<int> m_cpuUsageHistory;
    static constexpr size_t CPU_HISTORY_SIZE = 1000;

    // Other member variables...
    OverlaySettings m_settings;
    bool m_showSettings = false;  // Toggle for settings panel

    // Add these function prototypes to your Overlay class
    void RenderSettingsPanel();
    void SaveSettings();
    void LoadSettings();
    std::string GetSettingsFilePath();

    // Audio control related variables and methods
    IMMDeviceEnumerator* m_pEnumerator = nullptr;
    IMMDevice* m_pDevice = nullptr;
    IAudioEndpointVolume* m_pEndpointVolume = nullptr;
    std::vector<std::pair<std::string, std::string>> m_audioDevices;
    int m_selectedAudioDevice = 0;
    
    void InitializeAudio();
    void CleanupAudio();
    void RefreshAudioDevices();
    void SetAudioDevice(int deviceIndex);
    float GetMasterVolume();
    void SetMasterVolume(float volume);
    bool IsMasterMuted();
    void SetMasterMuted(bool muted);

    // Battery info related functions
    bool GetBatteryStatus(int& batteryPercent, bool& isCharging, int& remainingMinutes);

    // Add this line to the existing private methods
    void RenderAudioPanel();

    // Add these member functions to your Overlay class

    // Add these member variables
    bool m_showAudioWindow = false;
    ImVec2 m_audioWindowPos = ImVec2(200, 200);  // Default position

private:
    // Audio window settings
    bool m_showAudioSettings = false;

    // Add the additional member variables and methods:
    bool m_mouseInsideAudioWindow = false;
    void RenderAudioSettingsPanel();
public:
    // Add these public methods
    void ToggleAudioWindow();
    void RenderAudioWindow();
    
    // Add these to your class definition

    // Network settings struct (add to OverlaySettings or as a separate struct)
    struct NetworkSettings {
        bool showNetworkDetails = true;
        bool alwaysOnTop = false;
        bool savePosition = true;
    };

    // Add to OverlaySettings
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
        NetworkSettings networkSettings; // Add this line
    };

    // Add these member variables
private:
    bool m_showNetworkWindow = false;
    bool m_showNetworkSettings = false;
    bool m_mouseInsideNetworkWindow = false;
    ImVec2 m_networkWindowPos = ImVec2(300, 300);
    bool m_scanningNetworks = false;
    std::vector<std::pair<std::string, std::string>> m_availableNetworks; // Name, SSID
    std::string m_currentNetwork;
    bool m_isWifiEnabled = true;
    
    // Add these member functions
public:
    void ToggleNetworkWindow();
    
private:
    void RenderNetworkWindow();
    void RenderNetworkSettingsPanel();
    void ScanNetworks();
    void ToggleWifi(bool enable);
    std::string GetCurrentNetworkName();
    bool IsWifiEnabled();
};

// Global keyboard hook function
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);