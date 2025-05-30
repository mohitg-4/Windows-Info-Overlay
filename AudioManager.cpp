#include "AudioManager.h"
#include <functiondiscoverykeys_devpkey.h>

#pragma comment(lib, "Ole32.lib")

AudioManager::AudioManager() : 
    m_pEnumerator(nullptr),
    m_pDevice(nullptr),
    m_pEndpointVolume(nullptr),
    m_selectedDevice(0)
{
    Initialize();
}

AudioManager::~AudioManager()
{
    Cleanup();
}

void AudioManager::Initialize()
{
    // Initialize COM if not already initialized
    HRESULT hr = CoInitialize(nullptr);
    bool needsUninitialize = SUCCEEDED(hr);
    
    // Create device enumerator
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)&m_pEnumerator
    );
    
    if (SUCCEEDED(hr))
    {
        RefreshDevices();
        // Set to default device initially
        SetDevice(0);
    }
    
    if (needsUninitialize)
    {
        CoUninitialize();
    }
}

void AudioManager::Cleanup()
{
    if (m_pEndpointVolume)
    {
        m_pEndpointVolume->Release();
        m_pEndpointVolume = nullptr;
    }
    
    if (m_pDevice)
    {
        m_pDevice->Release();
        m_pDevice = nullptr;
    }
    
    if (m_pEnumerator)
    {
        m_pEnumerator->Release();
        m_pEnumerator = nullptr;
    }
}

void AudioManager::RefreshDevices()
{
    m_devices.clear();
    
    // Initialize COM for this thread if needed
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needsUninitialize = SUCCEEDED(hr) && hr != S_FALSE;
    
    if (!m_pEnumerator) 
    {
        // Try to create the enumerator if it doesn't exist
        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            (void**)&m_pEnumerator
        );
        
        if (FAILED(hr))
        {
            if (needsUninitialize) CoUninitialize();
            return;
        }
    }
    
    // Add the default device at index 0
    m_devices.push_back(std::make_pair("Default Device", ""));
    
    // Enumerate all audio rendering devices
    IMMDeviceCollection* pCollection = nullptr;
    hr = m_pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    
    if (SUCCEEDED(hr))
    {
        UINT count;
        hr = pCollection->GetCount(&count);
        
        if (SUCCEEDED(hr))
        {
            for (UINT i = 0; i < count; i++)
            {
                IMMDevice* pDevice = nullptr;
                hr = pCollection->Item(i, &pDevice);
                
                if (SUCCEEDED(hr))
                {
                    LPWSTR pwszID = nullptr;
                    hr = pDevice->GetId(&pwszID);
                    
                    if (SUCCEEDED(hr))
                    {
                        // Convert LPWSTR to std::string
                        int size_needed = WideCharToMultiByte(CP_UTF8, 0, pwszID, -1, nullptr, 0, nullptr, nullptr);
                        std::string deviceId(size_needed, 0);
                        WideCharToMultiByte(CP_UTF8, 0, pwszID, -1, &deviceId[0], size_needed, nullptr, nullptr);
                        
                        // Get device friendly name
                        IPropertyStore* pProps = nullptr;
                        hr = pDevice->OpenPropertyStore(STGM_READ, &pProps);
                        
                        if (SUCCEEDED(hr))
                        {
                            PROPVARIANT varName;
                            PropVariantInit(&varName);
                            
                            hr = pProps->GetValue(PKEY_Device_FriendlyName_Custom, &varName);
                            
                            if (SUCCEEDED(hr))
                            {
                                // Convert LPWSTR to std::string
                                int name_size = WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                                std::string deviceName(name_size, 0);
                                WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, &deviceName[0], name_size, nullptr, nullptr);
                                
                                m_devices.push_back(std::make_pair(deviceName, deviceId));
                                
                                PropVariantClear(&varName);
                            }
                            
                            pProps->Release();
                        }
                        
                        CoTaskMemFree(pwszID);
                    }
                    
                    pDevice->Release();
                }
            }
        }
        
        pCollection->Release();
    }
    
    if (needsUninitialize)
    {
        CoUninitialize();
    }
}

void AudioManager::SetDevice(int deviceIndex)
{
    // Initialize COM for this thread if needed
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needsUninitialize = SUCCEEDED(hr) && hr != S_FALSE;
    
    // Clean up previous device and endpoint
    if (m_pEndpointVolume)
    {
        m_pEndpointVolume->Release();
        m_pEndpointVolume = nullptr;
    }
    
    if (m_pDevice)
    {
        m_pDevice->Release();
        m_pDevice = nullptr;
    }
    
    // Safety check for enumerator
    if (!m_pEnumerator)
    {
        // Try to create the enumerator if it doesn't exist
        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            (void**)&m_pEnumerator
        );
        
        if (FAILED(hr))
        {
            if (needsUninitialize) CoUninitialize();
            return;
        }
    }
    
    // Safety check for device index and audio devices vector
    if (deviceIndex < 0 || m_devices.empty() || deviceIndex >= static_cast<int>(m_devices.size()))
    {
        if (m_devices.empty())
        {
            RefreshDevices();
            // If still empty, just exit
            if (m_devices.empty())
            {
                if (needsUninitialize) CoUninitialize();
                return;
            }
        }
        
        // Use default device (index 0) if requested index is invalid
        deviceIndex = 0;
    }
    
    m_selectedDevice = deviceIndex;
    
    // If default device (index 0), get the default device
    if (deviceIndex == 0)
    {
        hr = m_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_pDevice);
    }
    else
    {
        // Get device by ID
        const std::string& deviceId = m_devices[deviceIndex].second;
        
        // Convert std::string to LPWSTR
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, nullptr, 0);
        std::wstring wDeviceId(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, &wDeviceId[0], size_needed);
        
        hr = m_pEnumerator->GetDevice(wDeviceId.c_str(), &m_pDevice);
    }
    
    if (SUCCEEDED(hr) && m_pDevice)
    {
        hr = m_pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&m_pEndpointVolume);
    }
    
    if (needsUninitialize)
    {
        CoUninitialize();
    }
}

float AudioManager::GetMasterVolume()
{
    float level = 0.0f;
    
    // Initialize COM for this thread if needed
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needsUninitialize = SUCCEEDED(hr) && hr != S_FALSE;
    
    if (m_pEndpointVolume)
    {
        hr = m_pEndpointVolume->GetMasterVolumeLevelScalar(&level);
        if (FAILED(hr))
        {
            // If getting volume fails, try refreshing the audio device
            SetDevice(m_selectedDevice);
            if (m_pEndpointVolume)
            {
                m_pEndpointVolume->GetMasterVolumeLevelScalar(&level);
            }
        }
    }
    
    if (needsUninitialize)
    {
        CoUninitialize();
    }
    
    return level;
}

void AudioManager::SetMasterVolume(float volume)
{
    // Initialize COM for this thread if needed
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needsUninitialize = SUCCEEDED(hr) && hr != S_FALSE;
    
    if (m_pEndpointVolume)
    {
        // Make sure volume is in the valid range
        volume = (volume < 0.0f) ? 0.0f : (volume > 1.0f) ? 1.0f : volume;
        
        hr = m_pEndpointVolume->SetMasterVolumeLevelScalar(volume, nullptr);
        if (FAILED(hr))
        {
            // If setting volume fails, try refreshing the audio device
            SetDevice(m_selectedDevice);
            if (m_pEndpointVolume)
            {
                m_pEndpointVolume->SetMasterVolumeLevelScalar(volume, nullptr);
            }
        }
    }
    
    if (needsUninitialize)
    {
        CoUninitialize();
    }
}

bool AudioManager::IsMasterMuted()
{
    BOOL muted = FALSE;
    
    // Initialize COM for this thread if needed
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needsUninitialize = SUCCEEDED(hr) && hr != S_FALSE;
    
    if (m_pEndpointVolume)
    {
        hr = m_pEndpointVolume->GetMute(&muted);
        if (FAILED(hr))
        {
            // If getting mute state fails, try refreshing the audio device
            SetDevice(m_selectedDevice);
            if (m_pEndpointVolume)
            {
                m_pEndpointVolume->GetMute(&muted);
            }
        }
    }
    
    if (needsUninitialize)
    {
        CoUninitialize();
    }
    
    return muted != FALSE;
}

void AudioManager::SetMasterMuted(bool muted)
{
    // Initialize COM for this thread if needed
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needsUninitialize = SUCCEEDED(hr) && hr != S_FALSE;
    
    if (m_pEndpointVolume)
    {
        hr = m_pEndpointVolume->SetMute(muted, nullptr);
        if (FAILED(hr))
        {
            // If setting mute fails, try refreshing the audio device
            SetDevice(m_selectedDevice);
            if (m_pEndpointVolume)
            {
                m_pEndpointVolume->SetMute(muted, nullptr);
            }
        }
    }
    
    if (needsUninitialize)
    {
        CoUninitialize();
    }
}