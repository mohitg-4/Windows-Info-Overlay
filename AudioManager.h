#pragma once

#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <vector>
#include <string>

// Define the property key manually for MinGW compatibility
const static PROPERTYKEY PKEY_Device_FriendlyName_Custom = 
{ { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

// Settings for audio
struct AudioSettings {
    bool showVolumePercentage = true;
    bool showDeviceSelector = true;
    bool alwaysOnTop = false;
    bool savePosition = true;
};

class AudioManager {
public:
    AudioManager();
    ~AudioManager();

    // Initialization and cleanup
    void Initialize();
    void Cleanup();

    // Device management
    void RefreshDevices();
    void SetDevice(int deviceIndex);
    int GetSelectedDeviceIndex() const { return m_selectedDevice; }
    const std::vector<std::pair<std::string, std::string>>& GetDevices() const { return m_devices; }

    // Volume control
    float GetMasterVolume();
    void SetMasterVolume(float volume);
    bool IsMasterMuted();
    void SetMasterMuted(bool muted);

private:
    // COM interfaces for audio
    IMMDeviceEnumerator* m_pEnumerator = nullptr;
    IMMDevice* m_pDevice = nullptr;
    IAudioEndpointVolume* m_pEndpointVolume = nullptr;

    // Device list and selection
    std::vector<std::pair<std::string, std::string>> m_devices;  // Name, ID
    int m_selectedDevice = 0;
};