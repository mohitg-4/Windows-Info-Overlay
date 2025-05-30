#pragma once

#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audioclient.h>
#include <vector>
#include <string>
#include <thread>
#include <mutex>

// Define the property key manually for MinGW compatibility
const static PROPERTYKEY PKEY_Device_FriendlyName_Custom = 
{ { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

// Settings for audio
struct AudioSettings {
    bool showVolumePercentage = true;
    bool showDeviceSelector = true;
    bool showVisualizer = true;
    int visualizerStyle = 0;  // 0 = bars, 1 = line, 2 = circle
    float visualizerSensitivity = 1.0f;
    bool alwaysOnTop = false;
    bool savePosition = true;
};

// Number of frequency bands for the visualizer
const int VISUALIZER_BANDS = 32;

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

    // Visualizer
    void StartVisualizerCapture();
    void StopVisualizerCapture();
    bool IsVisualizerActive() const { return m_visualizerActive; }
    const std::vector<float>& GetVisualizerData() const { return m_visualizerData; }
    void UpdateVisualizerSettings(float sensitivity, int style);

private:
    // COM interfaces for audio
    IMMDeviceEnumerator* m_pEnumerator = nullptr;
    IMMDevice* m_pDevice = nullptr;
    IAudioEndpointVolume* m_pEndpointVolume = nullptr;

    // Device list and selection
    std::vector<std::pair<std::string, std::string>> m_devices;  // Name, ID
    int m_selectedDevice = 0;

    // Visualizer components
    bool m_visualizerActive = false;
    std::vector<float> m_visualizerData;
    std::vector<float> m_visualizerPeaks;
    std::vector<float> m_visualizerPeakFalloff;
    std::thread m_captureThread;
    std::mutex m_visualizerMutex;
    bool m_stopCapture = false;
    float m_sensitivity = 1.0f;
    int m_visualizerStyle = 0;

    // Audio capture for visualizer
    void VisualizerCaptureThread();
    void ProcessAudioData(const float* data, size_t frameCount, size_t channels);
    void CalculateFFT(const std::vector<float>& input, std::vector<float>& output);
};