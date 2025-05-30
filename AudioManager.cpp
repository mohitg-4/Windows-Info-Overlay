#include "AudioManager.h"
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <cmath>
#include <algorithm>
#define _USE_MATH_DEFINES
#include <math.h>

#pragma comment(lib, "Ole32.lib")

// Make sure there's only one constructor definition
AudioManager::AudioManager() : 
    m_pEnumerator(nullptr),
    m_pDevice(nullptr),
    m_pEndpointVolume(nullptr),
    m_selectedDevice(0),
    m_visualizerActive(false),
    m_stopCapture(false),
    m_sensitivity(1.0f),
    m_visualizerStyle(0)
{
    // Initialize visualizer data
    m_visualizerData.resize(VISUALIZER_BANDS, 0.0f);
    m_visualizerPeaks.resize(VISUALIZER_BANDS, 0.0f);
    m_visualizerPeakFalloff.resize(VISUALIZER_BANDS, 0.0f);
    
    // Initialize audio system
    Initialize();
}

// Make sure there's only one destructor definition
AudioManager::~AudioManager()
{
    // Make sure visualizer is stopped before cleaning up
    StopVisualizerCapture();
    Cleanup();
}

// Add visualizer start function
void AudioManager::StartVisualizerCapture()
{
    if (m_visualizerActive)
        return;
        
    m_stopCapture = false;
    m_visualizerActive = true;
    
    // Start capture thread
    m_captureThread = std::thread(&AudioManager::VisualizerCaptureThread, this);
}

// Add visualizer stop function
void AudioManager::StopVisualizerCapture()
{
    if (!m_visualizerActive)
        return;
    
    // Signal thread to stop
    m_stopCapture = true;
    
    // Wait for thread to finish
    if (m_captureThread.joinable())
        m_captureThread.join();
    
    m_visualizerActive = false;
    
    // Clear visualization data
    std::lock_guard<std::mutex> lock(m_visualizerMutex);
    std::fill(m_visualizerData.begin(), m_visualizerData.end(), 0.0f);
    std::fill(m_visualizerPeaks.begin(), m_visualizerPeaks.end(), 0.0f);
}

// Update settings for visualizer
void AudioManager::UpdateVisualizerSettings(float sensitivity, int style)
{
    m_sensitivity = sensitivity;
    m_visualizerStyle = style;
}

// The main audio capture thread function
void AudioManager::VisualizerCaptureThread()
{
    // Initialize COM for this thread
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        m_visualizerActive = false;
        return;
    }
    
    // Get the current device
    IMMDevice* pDevice = nullptr;
    if (!m_pEnumerator) {
        // Create the device enumerator if it doesn't exist
        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            (void**)&m_pEnumerator
        );
    }
    
    if (FAILED(hr) || !m_pEnumerator) {
        CoUninitialize();
        m_visualizerActive = false;
        return;
    }
    
    // Get the same device we're controlling
    if (m_selectedDevice == 0) {
        hr = m_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    } else {
        // Use the selected device
        if (m_selectedDevice < m_devices.size()) {
            const std::string& deviceId = m_devices[m_selectedDevice].second;
            
            // Convert to wide string
            int size_needed = MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, nullptr, 0);
            std::wstring wDeviceId(size_needed, 0);
            MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, &wDeviceId[0], size_needed);
            
            hr = m_pEnumerator->GetDevice(wDeviceId.c_str(), &pDevice);
        }
    }
    
    if (FAILED(hr) || !pDevice) {
        // Try default device as fallback
        hr = m_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
        if (FAILED(hr) || !pDevice) {
            CoUninitialize();
            m_visualizerActive = false;
            return;
        }
    }
    
    // Activate audio client interface
    IAudioClient* pAudioClient = nullptr;
    hr = pDevice->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL,
        NULL,
        (void**)&pAudioClient
    );
    
    if (FAILED(hr) || !pAudioClient) {
        if (pDevice) pDevice->Release();
        CoUninitialize();
        m_visualizerActive = false;
        return;
    }
    
    // Get audio format info
    WAVEFORMATEX* pwfx = nullptr;
    hr = pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr)) {
        pAudioClient->Release();
        pDevice->Release();
        CoUninitialize();
        m_visualizerActive = false;
        return;
    }
    
    // Initialize audio client for loopback capture (to listen to what's playing)
    const REFERENCE_TIME bufferDuration = 100000; // 10ms buffer
    hr = pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        bufferDuration,
        0,
        pwfx,
        NULL
    );
    
    if (FAILED(hr)) {
        CoTaskMemFree(pwfx);
        pAudioClient->Release();
        pDevice->Release();
        CoUninitialize();
        m_visualizerActive = false;
        return;
    }
    
    // Create capture client
    IAudioCaptureClient* pCaptureClient = nullptr;
    hr = pAudioClient->GetService(
        __uuidof(IAudioCaptureClient),
        (void**)&pCaptureClient
    );
    
    if (FAILED(hr) || !pCaptureClient) {
        CoTaskMemFree(pwfx);
        pAudioClient->Release();
        pDevice->Release();
        CoUninitialize();
        m_visualizerActive = false;
        return;
    }
    
    // Start capturing
    hr = pAudioClient->Start();
    if (FAILED(hr)) {
        pCaptureClient->Release();
        CoTaskMemFree(pwfx);
        pAudioClient->Release();
        pDevice->Release();
        CoUninitialize();
        m_visualizerActive = false;
        return;
    }
    
    // Capture loop
    const size_t channelCount = pwfx->nChannels;
    
    while (!m_stopCapture) {
        // Sleep to avoid spinning CPU
        Sleep(15); // ~66fps
        
        // Check if packets are available
        UINT32 packetLength = 0;
        hr = pCaptureClient->GetNextPacketSize(&packetLength);
        
        if (FAILED(hr)) {
            break;
        }
        
        // Process all available packets
        while (packetLength > 0) {
            // Get the data
            BYTE* pData = nullptr;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;
            
            hr = pCaptureClient->GetBuffer(
                &pData,
                &numFramesAvailable,
                &flags,
                NULL,
                NULL
            );
            
            if (FAILED(hr)) {
                break;
            }
            
            // Skip silent packets
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                // Process the captured audio data
                ProcessAudioData(reinterpret_cast<float*>(pData), numFramesAvailable, channelCount);
            }
            
            // Release the buffer
            hr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
            if (FAILED(hr)) {
                break;
            }
            
            // Get the size of the next packet
            hr = pCaptureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                break;
            }
        }
    }
    
    // Stop and clean up
    pAudioClient->Stop();
    pCaptureClient->Release();
    CoTaskMemFree(pwfx);
    pAudioClient->Release();
    pDevice->Release();
    CoUninitialize();
}

// Process audio data to create visualization
void AudioManager::ProcessAudioData(const float* data, size_t frameCount, size_t channels)
{
    if (frameCount == 0 || !data)
        return;
        
    // We'll use a simple approach - compute average amplitude in frequency bands
    // This is a simplified version without true FFT
    
    std::vector<float> bandValues(VISUALIZER_BANDS, 0.0f);
    
    // Calculate total amplitude across all frames for each band
    for (size_t i = 0; i < frameCount; i++) {
        // Get the mono mix of all channels for this frame
        float sample = 0.0f;
        for (size_t ch = 0; ch < channels; ch++) {
            sample += std::abs(data[i * channels + ch]);
        }
        sample /= channels;
        
        // Simple approach: distribute samples across bands based on position
        // This isn't frequency analysis but gives a visual effect
        size_t bandIndex = (i * VISUALIZER_BANDS) / frameCount;
        if (bandIndex < VISUALIZER_BANDS) {
            bandValues[bandIndex] += sample;
        }
    }
    
    // Normalize and apply sensitivity
    std::vector<float> normalizedBands(VISUALIZER_BANDS);
    for (size_t i = 0; i < VISUALIZER_BANDS; i++) {
        // Normalize by the number of samples in this band
        float samplesPerBand = static_cast<float>(frameCount) / VISUALIZER_BANDS;
        float value = bandValues[i] / samplesPerBand;
        
        // Apply sensitivity - higher sensitivity makes quieter sounds more visible
        value = std::min(1.0f, value * m_sensitivity * 5.0f);
        
        normalizedBands[i] = value;
    }
    
    // Update the visualizer data with mutex protection
    {
        std::lock_guard<std::mutex> lock(m_visualizerMutex);
        
        for (size_t i = 0; i < VISUALIZER_BANDS; i++) {
            // Smooth transition to new value (30% new, 70% old)
            m_visualizerData[i] = m_visualizerData[i] * 0.7f + normalizedBands[i] * 0.3f;
            
            // Update peaks
            if (m_visualizerData[i] > m_visualizerPeaks[i]) {
                m_visualizerPeaks[i] = m_visualizerData[i];
                m_visualizerPeakFalloff[i] = 0.002f; // Reset falloff speed
            } else {
                // Gradually reduce peak markers
                m_visualizerPeaks[i] -= m_visualizerPeakFalloff[i];
                m_visualizerPeakFalloff[i] *= 1.02f; // Accelerate falloff
                
                // Make sure peaks don't go below the current level
                if (m_visualizerPeaks[i] < m_visualizerData[i])
                    m_visualizerPeaks[i] = m_visualizerData[i];
                
                // Ensure it doesn't go negative
                if (m_visualizerPeaks[i] < 0.0f)
                    m_visualizerPeaks[i] = 0.0f;
            }
        }
    }
}

// Simple FFT implementation (simplified for visualization purposes)
void AudioManager::CalculateFFT(const std::vector<float>& input, std::vector<float>& output)
{
    // For a real FFT implementation, you'd want to use a library like FFTW or KissFFT
    // This is just a placeholder implementation that simulates frequency analysis
    
    const size_t N = input.size();
    output.resize(N/2);
    
    for (size_t k = 0; k < N/2; k++) {
        float real = 0.0f;
        float imag = 0.0f;
        
        for (size_t n = 0; n < N; n++) {
            float angle = 2.0f * M_PI * k * n / N;
            real += input[n] * std::cos(angle);
            imag -= input[n] * std::sin(angle);
        }
        
        // Magnitude
        output[k] = std::sqrt(real*real + imag*imag) / N;
    }
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