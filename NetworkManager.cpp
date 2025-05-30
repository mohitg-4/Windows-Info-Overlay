#include "NetworkManager.h"
#include <wlanapi.h>
#include <objbase.h>
#include <wtypes.h>
#pragma comment(lib, "wlanapi.lib")
#pragma comment(lib, "ole32.lib")

NetworkManager::NetworkManager() :
    m_lastInBytes(0),
    m_lastOutBytes(0),
    m_lastTickCount(0),
    m_downloadSpeed(0.0f),
    m_uploadSpeed(0.0f),
    m_wifiEnabled(true),
    m_scanning(false)
{
    // Initialize current network info
    m_currentNetwork = GetCurrentNetworkName();
}

NetworkManager::~NetworkManager()
{
    // Nothing to clean up
}

float NetworkManager::GetDownloadSpeed()
{
    UpdateSpeeds();
    return m_downloadSpeed;
}

float NetworkManager::GetUploadSpeed()
{
    UpdateSpeeds();
    return m_uploadSpeed;
}

void NetworkManager::UpdateSpeeds()
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

std::string NetworkManager::GetCurrentNetworkName()
{
    // Try to get WLAN connection information first (more accurate for WiFi)
    DWORD dwResult = 0;
    DWORD dwMaxClient = 2;
    DWORD dwCurVersion = 0;
    HANDLE hClient = NULL;
    std::string result = "Not Connected";
    
    // Open WLAN API handle
    dwResult = WlanOpenHandle(dwMaxClient, NULL, &dwCurVersion, &hClient);
    if (dwResult == ERROR_SUCCESS)
    {
        // Get interface list
        WLAN_INTERFACE_INFO_LIST* pIfList = NULL;
        dwResult = WlanEnumInterfaces(hClient, NULL, &pIfList);
        if (dwResult == ERROR_SUCCESS)
        {
            // Go through each interface
            for (DWORD i = 0; i < pIfList->dwNumberOfItems; i++)
            {
                WLAN_INTERFACE_INFO* pIfInfo = &pIfList->InterfaceInfo[i];
                
                // Only look at connected interfaces
                if (pIfInfo->isState == wlan_interface_state_connected)
                {
                    // Get connection attributes to get SSID
                    WLAN_CONNECTION_ATTRIBUTES* pConnAttr = NULL;
                    DWORD dwSize = 0;
                    dwResult = WlanQueryInterface(hClient,
                                                 &pIfInfo->InterfaceGuid,
                                                 wlan_intf_opcode_current_connection,
                                                 NULL,
                                                 &dwSize,
                                                 reinterpret_cast<PVOID*>(&pConnAttr),
                                                 NULL);
                                                 
                    if (dwResult == ERROR_SUCCESS)
                    {
                        // Extract SSID
                        if (pConnAttr->wlanAssociationAttributes.dot11Ssid.uSSIDLength > 0)
                        {
                            result.assign(reinterpret_cast<const char*>(
                                        pConnAttr->wlanAssociationAttributes.dot11Ssid.ucSSID),
                                        pConnAttr->wlanAssociationAttributes.dot11Ssid.uSSIDLength);
                            
                            // Update member variable
                            m_currentNetwork = result;
                            m_wifiEnabled = true;
                            
                            // Clean up and return
                            WlanFreeMemory(pConnAttr);
                            WlanFreeMemory(pIfList);
                            WlanCloseHandle(hClient, NULL);
                            return result;
                        }
                        
                        WlanFreeMemory(pConnAttr);
                    }
                }
            }
            
            WlanFreeMemory(pIfList);
        }
        
        WlanCloseHandle(hClient, NULL);
    }
    
    // Fall back to IP Helper API if WLAN API didn't find any connections
    ULONG bufferSize = 0;
    GetAdaptersInfo(NULL, &bufferSize);
    
    if (bufferSize == 0)
        return "Unknown";
    
    std::vector<BYTE> buffer(bufferSize);
    PIP_ADAPTER_INFO pAdapterInfo = reinterpret_cast<PIP_ADAPTER_INFO>(buffer.data());
    
    if (GetAdaptersInfo(pAdapterInfo, &bufferSize) != NO_ERROR)
        return "Unknown";
    
    bool foundConnection = false;
    
    // Loop through all adapters
    for (PIP_ADAPTER_INFO pAdapter = pAdapterInfo; pAdapter; pAdapter = pAdapter->Next)
    {
        // Skip adapters that are not connected or not Ethernet/Wireless
        if (pAdapter->Type != MIB_IF_TYPE_ETHERNET && 
            pAdapter->Type != IF_TYPE_IEEE80211)
            continue;
        
        // Check if adapter has IP address (is connected)
        if (pAdapter->IpAddressList.IpAddress.String[0] != '0')
        {
            // Store adapter description as network name
            result = pAdapter->Description;
            
            // For wireless adapters, try to get SSID (requires additional code)
            if (pAdapter->Type == IF_TYPE_IEEE80211)
            {
                // Try to find "Wi-Fi" or "Wireless" in description
                std::string desc = pAdapter->Description;
                if (desc.find("Wi-Fi") != std::string::npos ||
                    desc.find("Wireless") != std::string::npos)
                {
                    // Store in m_currentNetwork for later use
                    m_currentNetwork = result;
                    m_wifiEnabled = true;
                    return result;
                }
            }
            
            // Found a connected adapter
            foundConnection = true;
        }
    }
    
    if (foundConnection)
    {
        m_currentNetwork = result;
        return result;
    }
    
    // If we get here, no connected adapters were found
    m_wifiEnabled = false;
    m_currentNetwork = "Not Connected";
    return m_currentNetwork;
}

bool NetworkManager::IsWifiEnabled()
{
    // Check if any wireless adapters are enabled
    ULONG bufferSize = 0;
    GetAdaptersInfo(NULL, &bufferSize);
    
    if (bufferSize == 0)
        return false;  // No adapters
    
    std::vector<BYTE> buffer(bufferSize);
    PIP_ADAPTER_INFO pAdapterInfo = reinterpret_cast<PIP_ADAPTER_INFO>(buffer.data());
    
    if (GetAdaptersInfo(pAdapterInfo, &bufferSize) != NO_ERROR)
        return false;  // Error getting adapter info
    
    // Loop through adapters looking for wireless ones
    for (PIP_ADAPTER_INFO pAdapter = pAdapterInfo; pAdapter; pAdapter = pAdapter->Next)
    {
        // Check for wireless adapters
        if (pAdapter->Type == IF_TYPE_IEEE80211)
        {
            // If any wireless adapter has an IP address, consider WiFi enabled
            if (pAdapter->IpAddressList.IpAddress.String[0] != '0')
            {
                m_wifiEnabled = true;
                return true;
            }
        }
    }
    
    // Check our cached state as a fallback
    return m_wifiEnabled;
}

void NetworkManager::ScanNetworks()
{
    m_scanning = true;
    
    // Clear existing networks
    m_availableNetworks.clear();
    
    // Add the current network at the top
    std::string currentNet = GetCurrentNetworkName();
    if (currentNet != "Not Connected" && currentNet != "Unknown")
    {
        m_availableNetworks.push_back(std::make_pair(currentNet, "Current"));
    }
    
    // Initialize WLAN API
    DWORD dwResult = 0;
    DWORD dwMaxClient = 2;
    DWORD dwCurVersion = 0;
    HANDLE hClient = NULL;
    
    // Open WLAN API handle
    dwResult = WlanOpenHandle(dwMaxClient, NULL, &dwCurVersion, &hClient);
    if (dwResult != ERROR_SUCCESS)
    {
        // Fall back to adapter info method if WLAN API fails
        GetAdapterNetworks();
        m_scanning = false;
        return;
    }
    
    // Get interface list
    WLAN_INTERFACE_INFO_LIST* pIfList = NULL;
    dwResult = WlanEnumInterfaces(hClient, NULL, &pIfList);
    if (dwResult != ERROR_SUCCESS)
    {
        WlanCloseHandle(hClient, NULL);
        // Fall back to adapter info method if interface enumeration fails
        GetAdapterNetworks();
        m_scanning = false;
        return;
    }
    
    // Go through each interface
    for (DWORD i = 0; i < pIfList->dwNumberOfItems; i++)
    {
        WLAN_INTERFACE_INFO* pIfInfo = &pIfList->InterfaceInfo[i];
        
        // Scan for networks on this interface
        // This will not complete immediately and we'd need to register for notifications
        // to know when scanning completes, but we'll keep it simple
        WlanScan(hClient, &pIfInfo->InterfaceGuid, NULL, NULL, NULL);
        
        // Wait a bit for the scan to gather some results
        Sleep(1000);
        
        // Get list of available networks
        WLAN_AVAILABLE_NETWORK_LIST* pNetList = NULL;
        dwResult = WlanGetAvailableNetworkList(hClient, 
                                              &pIfInfo->InterfaceGuid,
                                              WLAN_AVAILABLE_NETWORK_INCLUDE_ALL_ADHOC_PROFILES,
                                              NULL,
                                              &pNetList);
                                              
        if (dwResult == ERROR_SUCCESS)
        {
            // Add each network to our list
            for (DWORD j = 0; j < pNetList->dwNumberOfItems; j++)
            {
                WLAN_AVAILABLE_NETWORK* pNet = &pNetList->Network[j];
                
                // Convert the SSID to a string (it's not null-terminated)
                std::string ssid;
                if (pNet->dot11Ssid.uSSIDLength > 0)
                {
                    ssid.assign(reinterpret_cast<const char*>(pNet->dot11Ssid.ucSSID), 
                               pNet->dot11Ssid.uSSIDLength);
                }
                else
                {
                    ssid = "<Hidden Network>";
                }
                
                // Format the signal quality as a percentage
                std::string signalInfo = std::to_string(pNet->wlanSignalQuality) + "% Signal";
                
                // Skip if we already have this network in our list
                if (std::find_if(m_availableNetworks.begin(), m_availableNetworks.end(),
                    [&ssid](const auto& pair) { return pair.first == ssid; }) != m_availableNetworks.end())
                {
                    continue;
                }
                
                // Add to our networks list
                m_availableNetworks.push_back(std::make_pair(ssid, signalInfo));
            }
            
            // Free the network list
            WlanFreeMemory(pNetList);
        }
    }
    
    // Clean up
    WlanFreeMemory(pIfList);
    WlanCloseHandle(hClient, NULL);
    
    // Sort networks by signal strength (better signal first)
    // We'd need to parse the signal info for this, for now we'll keep as is
    
    m_scanning = false;
}

// Helper method to get networks from adapter list
void NetworkManager::GetAdapterNetworks()
{
    // Get adapter information
    ULONG bufferSize = 0;
    GetAdaptersInfo(NULL, &bufferSize);
    
    if (bufferSize > 0)
    {
        std::vector<BYTE> buffer(bufferSize);
        PIP_ADAPTER_INFO pAdapterInfo = reinterpret_cast<PIP_ADAPTER_INFO>(buffer.data());
        
        if (GetAdaptersInfo(pAdapterInfo, &bufferSize) == NO_ERROR)
        {
            // Get all adapters
            for (PIP_ADAPTER_INFO pAdapter = pAdapterInfo; pAdapter; pAdapter = pAdapter->Next)
            {
                // Only process Ethernet and Wi-Fi adapters
                if (pAdapter->Type == MIB_IF_TYPE_ETHERNET || 
                    pAdapter->Type == IF_TYPE_IEEE80211)
                {
                    std::string adapterName = pAdapter->Description;
                    std::string adapterMAC;
                    
                    // Format MAC address
                    for (UINT i = 0; i < pAdapter->AddressLength; i++)
                    {
                        char hex[3];
                        sprintf(hex, "%02X", (int)pAdapter->Address[i]);
                        adapterMAC += hex;
                        if (i < pAdapter->AddressLength - 1)
                            adapterMAC += ":";
                    }
                    
                    // Skip if we already have this adapter in our list
                    if (std::find_if(m_availableNetworks.begin(), m_availableNetworks.end(),
                        [&adapterName](const auto& pair) { return pair.first == adapterName; }) != m_availableNetworks.end())
                    {
                        continue;
                    }
                    
                    // Add available wired networks too for completeness
                    m_availableNetworks.push_back(std::make_pair(adapterName, adapterMAC));
                }
            }
        }
    }
}

void NetworkManager::ToggleWifi(bool enable)
{
    m_scanning = true; // Set scanning flag to prevent UI interference
    
    // Use netsh command to enable/disable WiFi adapter
    std::string command = enable 
        ? "netsh interface set interface \"Wi-Fi\" enabled" 
        : "netsh interface set interface \"Wi-Fi\" disabled";
    
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    
    // Create the process to execute the command
    if (CreateProcessA(NULL, const_cast<LPSTR>(command.c_str()), NULL, NULL, FALSE, 
                     CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        // Wait for the process to finish
        WaitForSingleObject(pi.hProcess, 5000);  // Wait up to 5 seconds
        
        // Close process handles
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        
        // Update our internal state
        m_wifiEnabled = enable;
        
        // Update current network info
        if (!enable)
        {
            m_currentNetwork = "Not Connected";
        }
        else
        {
            // Give the system some time to reconnect
            Sleep(1500);
            m_currentNetwork = GetCurrentNetworkName();
        }
    }
    
    m_scanning = false;
}

bool NetworkManager::ConnectToNetwork(const std::string& ssid, const std::string& password)
{
    HANDLE hClient = NULL;
    DWORD dwMaxClient = 2;
    DWORD dwCurVersion = 0;
    DWORD dwResult = WlanOpenHandle(dwMaxClient, NULL, &dwCurVersion, &hClient);
    if (dwResult != ERROR_SUCCESS) return false;

    WLAN_INTERFACE_INFO_LIST* pIfList = NULL;
    dwResult = WlanEnumInterfaces(hClient, NULL, &pIfList);
    if (dwResult != ERROR_SUCCESS) {
        WlanCloseHandle(hClient, NULL);
        return false;
    }

    bool success = false;
    for (DWORD i = 0; i < pIfList->dwNumberOfItems; i++) {
        WLAN_INTERFACE_INFO* pIfInfo = &pIfList->InterfaceInfo[i];

        // Build profile XML for WPA2-Personal
        std::string profileXml =
            "<?xml version=\"1.0\"?>"
            "<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
            "<name>" + ssid + "</name>"
            "<SSIDConfig><SSID><name>" + ssid + "</name></SSID></SSIDConfig>"
            "<connectionType>ESS</connectionType>"
            "<connectionMode>auto</connectionMode>"
            "<MSM><security><authEncryption><authentication>WPA2PSK</authentication>"
            "<encryption>AES</encryption><useOneX>false</useOneX></authEncryption>"
            "<sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>" + password + "</keyMaterial></sharedKey>"
            "</security></MSM></WLANProfile>";

        DWORD dwReason;
        HRESULT hr = WlanSetProfile(
            hClient,
            &pIfInfo->InterfaceGuid,
            0,
            std::wstring(profileXml.begin(), profileXml.end()).c_str(),
            NULL,
            TRUE,
            NULL,
            &dwReason
        );
        if (hr != ERROR_SUCCESS) continue;

        WLAN_CONNECTION_PARAMETERS params = {};
        params.wlanConnectionMode = wlan_connection_mode_profile;
        params.strProfile = std::wstring(ssid.begin(), ssid.end()).c_str();
        params.pDot11Ssid = NULL;
        params.pDesiredBssidList = NULL;
        params.dot11BssType = dot11_BSS_type_any;
        params.dwFlags = 0;

        hr = WlanConnect(hClient, &pIfInfo->InterfaceGuid, &params, NULL);
        if (hr == ERROR_SUCCESS) {
            success = true;
            break;
        }
    }

    WlanFreeMemory(pIfList);
    WlanCloseHandle(hClient, NULL);
    return success;
}