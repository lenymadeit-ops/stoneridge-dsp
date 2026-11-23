#pragma once

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wil/com.h>

#include <cstdint>
#include <functional>
#include <string>

namespace stoneridge {

struct DeviceFormatInfo {
    uint32_t sampleRate       = 48000;
    uint32_t bitDepth         = 24;
    uint32_t validBits        = 24;
    uint32_t channels         = 2;
    bool     isFloat          = false;
    uint32_t supportedMask    = 0; // Bitmask: 0=44.1k, 1=48k, 2=88.2k, 3=96k, 4=192k
};

class WasapiDeviceManager : public IMMNotificationClient {
public:
    using DeviceChangeCallback = std::function<void()>;

    WasapiDeviceManager();
    ~WasapiDeviceManager();

    void SetDeviceChangeCallback(DeviceChangeCallback cb);
    void StartMonitoring();
    void StopMonitoring();

    // Finds the Mackie EM-91CU+ capture & render endpoints (or falls back to default endpoints).
    bool FindMackieEndpoints(wil::com_ptr<IMMDevice>& outCapture,
                             wil::com_ptr<IMMDevice>& outRender,
                             std::wstring& outFriendlyName,
                             bool& outIsMackie);

    // Probes the highest supported Exclusive-Mode format for the given sample rate.
    static bool ProbeBestExclusiveFormat(IAudioClient* client,
                                         uint32_t sampleRate,
                                         WAVEFORMATEXTENSIBLE& outFormat,
                                         uint32_t& outBitDepth);

    // Returns a bitmask of supported sample rates (44.1, 48, 88.2, 96, 192 kHz).
    static uint32_t ProbeSupportedSampleRates(IAudioClient* client);

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) override;

private:
    static std::wstring GetDeviceFriendlyName(IMMDevice* device);
    static bool ContainsMackieEm91Cu(const std::wstring& name);

    wil::com_ptr<IMMDeviceEnumerator> m_enumerator;
    DeviceChangeCallback m_onDeviceChange;
    bool m_monitoring = false;
    LONG m_refCount   = 1;
};

} // namespace stoneridge
