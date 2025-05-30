#pragma once

#include <string>
#include <vector>
#include <windows.h>
#include <iphlpapi.h>
#include <utility>
#include <wlanapi.h>
#include <algorithm>

// Settings for network
struct NetworkSettings {
    bool showNetworkDetails = true;
    bool alwaysOnTop = false;
    bool savePosition = true;
};

class NetworkManager {
public:
    NetworkManager();
    ~NetworkManager();

    // Network status
    float GetDownloadSpeed();
    float GetUploadSpeed();
    std::string GetCurrentNetworkName();
    bool IsWifiEnabled();

    // Network operations
    void ScanNetworks();
    void ToggleWifi(bool enable);
    
    // Update network statistics
    void UpdateSpeeds();
    
    // Get list of available networks
    const std::vector<std::pair<std::string, std::string>>& GetAvailableNetworks() const { return m_availableNetworks; }
    
    // Get scan status
    bool IsScanning() const { return m_scanning; }

private:
    // Network statistics
    ULONG64 m_lastInBytes = 0;
    ULONG64 m_lastOutBytes = 0;
    DWORD m_lastTickCount = 0;
    float m_downloadSpeed = 0.0f;
    float m_uploadSpeed = 0.0f;
    
    // Network state
    bool m_wifiEnabled = true;
    bool m_scanning = false;
    std::string m_currentNetwork;
    std::vector<std::pair<std::string, std::string>> m_availableNetworks;  // Name, SSID

    void GetAdapterNetworks(); // Helper method for ScanNetworks
};

#pragma comment(lib, "wlanapi.lib")