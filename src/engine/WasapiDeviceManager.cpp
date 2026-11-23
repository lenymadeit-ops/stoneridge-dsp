#include "engine/WasapiDeviceManager.h"

#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>

#include <algorithm>
#include <cwctype>

namespace stoneridge {

namespace {

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

WAVEFORMATEXTENSIBLE MakeWaveFormat(uint32_t sampleRate, uint16_t channels, uint16_t containerBits, uint16_t validBits, bool isFloat) {
    WAVEFORMATEXTENSIBLE wf{};
    wf.Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels            = channels;
    wf.Format.nSamplesPerSec       = sampleRate;
    wf.Format.wBitsPerSample       = containerBits;
    wf.Format.nBlockAlign          = static_cast<WORD>(channels * (containerBits / 8));
    wf.Format.nAvgBytesPerSec      = sampleRate * wf.Format.nBlockAlign;
    wf.Format.cbSize               = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wf.Samples.wValidBitsPerSample = validBits;
    wf.dwChannelMask               = (channels == 1) ? KSAUDIO_SPEAKER_MONO : KSAUDIO_SPEAKER_STEREO;
    wf.SubFormat                   = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    return wf;
}

} // namespace

WasapiDeviceManager::WasapiDeviceManager() {
    const HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&m_enumerator));
    if (FAILED(hr)) {
        m_enumerator.reset();
    }
}

WasapiDeviceManager::~WasapiDeviceManager() {
    StopMonitoring();
}

void WasapiDeviceManager::SetDeviceChangeCallback(DeviceChangeCallback cb) {
    m_onDeviceChange = std::move(cb);
}

void WasapiDeviceManager::StartMonitoring() {
    if (m_enumerator && !m_monitoring) {
        if (SUCCEEDED(m_enumerator->RegisterEndpointNotificationCallback(this))) {
            m_monitoring = true;
        }
    }
}

void WasapiDeviceManager::StopMonitoring() {
    if (m_enumerator && m_monitoring) {
        m_enumerator->UnregisterEndpointNotificationCallback(this);
        m_monitoring = false;
    }
}

std::wstring WasapiDeviceManager::GetDeviceFriendlyName(IMMDevice* device) {
    if (!device) return L"";
    wil::com_ptr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) return L"";

    PROPVARIANT var;
    PropVariantInit(&var);
    std::wstring result;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var))) {
        if (var.vt == VT_LPWSTR && var.pwszVal) {
            result = var.pwszVal;
        }
    }
    PropVariantClear(&var);
    return result;
}

bool WasapiDeviceManager::ContainsMackieEm91Cu(const std::wstring& name) {
    const auto lower = ToLower(name);
    return lower.find(L"em-91cu") != std::wstring::npos ||
           lower.find(L"em91cu")  != std::wstring::npos ||
           lower.find(L"mackie")  != std::wstring::npos;
}

bool WasapiDeviceManager::FindMackieEndpoints(wil::com_ptr<IMMDevice>& outCapture,
                                              wil::com_ptr<IMMDevice>& outRender,
                                              std::wstring& outFriendlyName,
                                              bool& outIsMackie) {
    outCapture.reset();
    outRender.reset();
    outFriendlyName.clear();
    outIsMackie = false;

    if (!m_enumerator) {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&m_enumerator))) || !m_enumerator) {
            return false;
        }
    }

    auto findInFlow = [&](EDataFlow flow, wil::com_ptr<IMMDevice>& target, std::wstring& nameOut) -> bool {
        wil::com_ptr<IMMDeviceCollection> col;
        if (FAILED(m_enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return false;

        UINT count = 0;
        col->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            wil::com_ptr<IMMDevice> dev;
            if (SUCCEEDED(col->Item(i, &dev))) {
                const auto name = GetDeviceFriendlyName(dev.get());
                if (ContainsMackieEm91Cu(name)) {
                    target = dev;
                    nameOut = name;
                    return true;
                }
            }
        }
        return false;
    };

    std::wstring capName, renName;
    const bool foundCap = findInFlow(eCapture, outCapture, capName);
    const bool foundRen = findInFlow(eRender, outRender, renName);

    if (foundCap || foundRen) {
        outIsMackie = true;
        outFriendlyName = foundCap ? capName : renName;
    }

    // Fallback to default active endpoints if Mackie is not found
    if (!outCapture) {
        m_enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &outCapture);
        if (outCapture && outFriendlyName.empty()) {
            outFriendlyName = GetDeviceFriendlyName(outCapture.get());
        }
    }
    if (!outRender) {
        m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &outRender);
        if (outRender && outFriendlyName.empty()) {
            outFriendlyName = GetDeviceFriendlyName(outRender.get());
        }
    }

    return static_cast<bool>(outCapture) || static_cast<bool>(outRender);
}

bool WasapiDeviceManager::ProbeBestExclusiveFormat(IAudioClient* client,
                                                   uint32_t sampleRate,
                                                   WAVEFORMATEXTENSIBLE& outFormat,
                                                   uint32_t& outBitDepth) {
    if (!client) return false;

    struct Candidate {
        uint16_t containerBits;
        uint16_t validBits;
        bool     isFloat;
        uint32_t reportedDepth;
    };

    // Priority: 32-bit float -> 32-bit int -> 24-bit in 32-bit -> 24-bit packed -> 16-bit
    constexpr Candidate kCandidates[] = {
        { 32, 32, true,  32 },
        { 32, 32, false, 32 },
        { 32, 24, false, 24 },
        { 24, 24, false, 24 },
        { 16, 16, false, 16 },
    };

    for (const uint16_t ch : { uint16_t{2}, uint16_t{1} }) {
        for (const auto& c : kCandidates) {
            auto wf = MakeWaveFormat(sampleRate, ch, c.containerBits, c.validBits, c.isFloat);
            const HRESULT hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                         reinterpret_cast<WAVEFORMATEX*>(&wf),
                                                         nullptr);
            if (hr == S_OK) {
                outFormat   = wf;
                outBitDepth = c.reportedDepth;
                return true;
            }
        }
    }
    return false;
}

uint32_t WasapiDeviceManager::ProbeSupportedSampleRates(IAudioClient* client) {
    if (!client) return 0;
    constexpr uint32_t kRates[] = { 44100, 48000, 88200, 96000, 192000 };
    uint32_t mask = 0;
    WAVEFORMATEXTENSIBLE dummy{};
    uint32_t dummyDepth = 0;
    for (uint32_t i = 0; i < 5; ++i) {
        if (ProbeBestExclusiveFormat(client, kRates[i], dummy, dummyDepth)) {
            mask |= (1u << i);
        }
    }
    return mask;
}

HRESULT STDMETHODCALLTYPE WasapiDeviceManager::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
        *ppvObject = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE WasapiDeviceManager::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
}

ULONG STDMETHODCALLTYPE WasapiDeviceManager::Release() {
    return static_cast<ULONG>(InterlockedDecrement(&m_refCount));
}

HRESULT STDMETHODCALLTYPE WasapiDeviceManager::OnDeviceStateChanged(LPCWSTR, DWORD) {
    if (m_onDeviceChange) m_onDeviceChange();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE WasapiDeviceManager::OnDeviceAdded(LPCWSTR) {
    if (m_onDeviceChange) m_onDeviceChange();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE WasapiDeviceManager::OnDeviceRemoved(LPCWSTR) {
    if (m_onDeviceChange) m_onDeviceChange();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE WasapiDeviceManager::OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE WasapiDeviceManager::OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) {
    return S_OK;
}

} // namespace stoneridge
