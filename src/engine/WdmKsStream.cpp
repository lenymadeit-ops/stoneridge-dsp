#include "engine/WdmKsStream.h"
#include "engine/AsioHostBridge.h"
#include "engine/AudioFormatConvert.h"
#include "engine/RealtimeGuard.h"

#include <avrt.h>
#include <setupapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "setupapi.lib")

namespace stoneridge {

namespace {

bool KsPropertyOverlapped(HANDLE h, const GUID& set, ULONG id, ULONG flags,
                          const void* inExtra, DWORD inExtraSize,
                          void* outBuf, DWORD outSize, DWORD* bytesRet) noexcept {
    if (!h || h == INVALID_HANDLE_VALUE || inExtraSize > 64) return false;
    BYTE propBuf[sizeof(KSPROPERTY) + 64]{};
    auto* prop  = reinterpret_cast<KSPROPERTY*>(propBuf);
    prop->Set   = set;
    prop->Id    = id;
    prop->Flags = flags;
    if (inExtra && inExtraSize > 0) {
        std::memcpy(propBuf + sizeof(KSPROPERTY), inExtra, inExtraSize);
    }

    wil::unique_event evt;
    evt.create(wil::EventOptions::ManualReset);
    if (!evt.is_valid()) return false;
    OVERLAPPED ov{};
    ov.hEvent = evt.get();

    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_KS_PROPERTY,
                              propBuf, static_cast<DWORD>(sizeof(KSPROPERTY) + inExtraSize),
                              outBuf, outSize, &ret, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = GetOverlappedResult(h, &ov, &ret, TRUE);
    }
    if (bytesRet) *bytesRet = ret;
    return ok != FALSE;
}

} // namespace

WdmKsStream::WdmKsStream() {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    m_qpcFreq = static_cast<uint64_t>(freq.QuadPart);

    m_ksUserDll = LoadLibraryW(L"ksuser.dll");
    if (m_ksUserDll) {
        m_ksCreatePin = reinterpret_cast<KsCreatePinFn>(GetProcAddress(m_ksUserDll, "KsCreatePin"));
    }
}

WdmKsStream::~WdmKsStream() {
    Stop();
    DisposeBuffers();
    ShutdownDevices();
    if (m_ksUserDll) {
        FreeLibrary(m_ksUserDll);
        m_ksUserDll = nullptr;
    }
}

bool WdmKsStream::FindMackieKsFilter(std::wstring& outPath, ULONG& outRenderPinId, ULONG& outCapturePinId) {
    outPath.clear();
    outRenderPinId  = UINT32_MAX;
    outCapturePinId = UINT32_MAX;

    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&KSCATEGORY_AUDIO, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool found = false;
    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(ifData);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, &KSCATEGORY_AUDIO, i, &ifData); ++i) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, nullptr, 0, &reqSize, nullptr);
        if (reqSize == 0) continue;

        std::vector<BYTE> buf(reqSize);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, detail, reqSize, nullptr, nullptr)) {
            continue;
        }

        std::wstring path(detail->DevicePath);
        if (path.find(L"vid_0a73") == std::wstring::npos && path.find(L"VID_0A73") == std::wstring::npos) {
            continue;
        }

        wil::unique_handle hFilter(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (!hFilter) continue;

        ULONG pinCount = 0;
        DWORD bytes = 0;
        if (!KsPropertyOverlapped(hFilter.get(), KSPROPSETID_Pin, KSPROPERTY_PIN_CTYPES,
                                  KSPROPERTY_TYPE_GET, nullptr, 0, &pinCount, sizeof(pinCount), &bytes)) {
            continue;
        }

        uint32_t ratesMask = 0;
        for (ULONG pinId = 0; pinId < pinCount; ++pinId) {
            KSPIN_DATAFLOW dataflow{};
            KSPIN_COMMUNICATION comm{};
            ULONG extra[2] = { pinId, 0 };
            KsPropertyOverlapped(hFilter.get(), KSPROPSETID_Pin, KSPROPERTY_PIN_DATAFLOW,
                                 KSPROPERTY_TYPE_GET, extra, sizeof(extra), &dataflow, sizeof(dataflow), &bytes);
            KsPropertyOverlapped(hFilter.get(), KSPROPSETID_Pin, KSPROPERTY_PIN_COMMUNICATION,
                                 KSPROPERTY_TYPE_GET, extra, sizeof(extra), &comm, sizeof(comm), &bytes);

            if (comm != KSPIN_COMMUNICATION_SINK && comm != KSPIN_COMMUNICATION_BOTH) {
                continue;
            }

            DWORD rangeBytes = 0;
            KsPropertyOverlapped(hFilter.get(), KSPROPSETID_Pin, KSPROPERTY_PIN_DATARANGES,
                                 KSPROPERTY_TYPE_GET, extra, sizeof(extra), nullptr, 0, &rangeBytes);
            if (rangeBytes <= sizeof(KSMULTIPLE_ITEM)) continue;

            std::vector<BYTE> rbuf(rangeBytes);
            if (!KsPropertyOverlapped(hFilter.get(), KSPROPSETID_Pin, KSPROPERTY_PIN_DATARANGES,
                                      KSPROPERTY_TYPE_GET, extra, sizeof(extra), rbuf.data(), rangeBytes, &rangeBytes)) {
                continue;
            }

            bool hasPcm = false;
            const auto* multi = reinterpret_cast<const KSMULTIPLE_ITEM*>(rbuf.data());
            const BYTE* ptr = rbuf.data() + sizeof(KSMULTIPLE_ITEM);
            for (ULONG r = 0; r < multi->Count; ++r) {
                const auto* dr = reinterpret_cast<const KSDATARANGE*>(ptr);
                if (dr->MajorFormat == KSDATAFORMAT_TYPE_AUDIO &&
                    dr->SubFormat == KSDATAFORMAT_SUBTYPE_PCM &&
                    dr->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
                    hasPcm = true;
                    const auto* aud = reinterpret_cast<const KSDATARANGE_AUDIO*>(dr);
                    constexpr uint32_t kRates[] = { 44100, 48000, 88200, 96000, 192000 };
                    for (uint32_t ri = 0; ri < 5; ++ri) {
                        if (kRates[ri] >= aud->MinimumSampleFrequency &&
                            kRates[ri] <= aud->MaximumSampleFrequency) {
                            ratesMask |= (1u << ri);
                        }
                    }
                }
                ptr += (dr->FormatSize + 7) & ~7;
            }

            if (hasPcm) {
                if (dataflow == KSPIN_DATAFLOW_IN && outRenderPinId == UINT32_MAX) {
                    outRenderPinId = pinId;
                } else if (dataflow == KSPIN_DATAFLOW_OUT && outCapturePinId == UINT32_MAX) {
                    outCapturePinId = pinId;
                }
            }
        }

        if (outRenderPinId != UINT32_MAX) {
            outPath = path;
            m_supportedRatesMask = ratesMask ? ratesMask : 0b01011;
            found = true;
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return found;
}

bool WdmKsStream::InitializeDevices(uint32_t sampleRate, uint32_t preferredBufferSize, SharedState* sharedState) {
    m_sharedState = sharedState;
    m_sampleRate  = static_cast<double>(sampleRate);
    m_bufferSize  = std::clamp(preferredBufferSize, 32u, 1024u);

    if (!m_ksCreatePin) {
        return false;
    }

    if (!FindMackieKsFilter(m_filterPath, m_renderPinId, m_capturePinId)) {
        return false;
    }

    m_filterHandle.reset(CreateFileW(m_filterPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (!m_filterHandle) {
        return false;
    }

    // Probe Hardware Topology, Mono Pin 1 support, WaveRT, and USB Audio Feature Unit PGA/DAC Gains
    m_hwController.Initialize(m_filterPath, m_capturePinId, m_renderPinId);
    const auto& caps = m_hwController.GetCapabilities();

    m_hidMonitor.Start(
        m_sharedState,
        [this](bool muted) {
            m_hwController.SetHardwareMute(muted);
        },
        [this](float deltaDb) {
            if (m_sharedState) {
                const auto& c = m_hwController.GetCapabilities();
                const float cur = m_sharedState->hardwareDacGainDb.load(std::memory_order_relaxed);
                const float next = std::clamp(cur + deltaDb, c.outputDacMinDb, c.outputDacMaxDb);
                const float bal = m_sharedState->hardwareDacBalanceDb.load(std::memory_order_relaxed);
                if (m_hwController.SetOutputDacGainAndBalanceDb(next, bal)) {
                    m_sharedState->hardwareDacGainDb.store(m_hwController.GetCapabilities().outputDacCurrentDb,
                                                           std::memory_order_relaxed);
                }
            }
        });

    m_bitDepth    = (sampleRate >= 88200) ? 16 : 24;
    m_inChannels  = (m_capturePinId != UINT32_MAX) ? 2 : 0;
    m_outChannels = 2;

    if (m_sharedState) {
        m_sharedState->deviceConnected.store(true, std::memory_order_relaxed);
        m_sharedState->activeEngineMode.store(static_cast<uint32_t>(EngineMode::WdmKsDirect), std::memory_order_relaxed);
        m_sharedState->supportedSampleRatesMask.store(m_supportedRatesMask, std::memory_order_relaxed);
        m_sharedState->activeSampleRate.store(m_sampleRate, std::memory_order_relaxed);
        m_sharedState->activeBitDepth.store(m_bitDepth, std::memory_order_relaxed);
        m_sharedState->inputChannels.store(m_inChannels, std::memory_order_relaxed);
        m_sharedState->outputChannels.store(m_outChannels, std::memory_order_relaxed);

        m_sharedState->hasHardwarePga.store(caps.hasInputPgaGain, std::memory_order_relaxed);
        m_sharedState->hardwarePgaMinDb.store(caps.inputPgaMinDb, std::memory_order_relaxed);
        m_sharedState->hardwarePgaMaxDb.store(caps.inputPgaMaxDb, std::memory_order_relaxed);
        m_sharedState->hardwarePgaGainDb.store(caps.inputPgaCurrentDb, std::memory_order_relaxed);

        m_sharedState->hasHardwareDac.store(caps.hasOutputDacGain, std::memory_order_relaxed);
        m_sharedState->hardwareDacMinDb.store(caps.outputDacMinDb, std::memory_order_relaxed);
        m_sharedState->hardwareDacMaxDb.store(caps.outputDacMaxDb, std::memory_order_relaxed);
        m_sharedState->hardwareDacGainDb.store(caps.outputDacCurrentDb, std::memory_order_relaxed);
        const float savedBal = m_sharedState->hardwareDacBalanceDb.load(std::memory_order_relaxed);
        if (caps.hasOutputDacGain && std::fabs(savedBal) >= 0.25f) {
            m_hwController.SetOutputDacGainAndBalanceDb(caps.outputDacCurrentDb, savedBal);
        }

        m_sharedState->hardwareMuted.store(caps.hardwareMuted, std::memory_order_relaxed);
        m_sharedState->supportsWaveRt.store(caps.supportsWaveRt, std::memory_order_relaxed);
        m_lastHwCtrlCounter = m_sharedState->hardwareCtrlCounter.load(std::memory_order_relaxed);
        m_adcClipConfirmStreak = 0;
        m_lastAutoPadTickMs    = 0;

        std::snprintf(m_sharedState->deviceName, sizeof(m_sharedState->deviceName), "%s", "Mackie EM-91CU+ (WDM-KS Direct)");
    }

    return true;
}

void WdmKsStream::ShutdownDevices() {
    m_hidMonitor.Stop();
    m_hwController.Shutdown();
    DisposeBuffers();
    m_filterHandle.reset();
}

void WdmKsStream::SyncHardwareControls() {
    if (!m_sharedState) return;

    const uint32_t cnt = m_sharedState->hardwareCtrlCounter.load(std::memory_order_acquire);
    if (cnt != m_lastHwCtrlCounter) {
        m_lastHwCtrlCounter = cnt;
        if (m_hwController.SetInputPgaGainDb(m_sharedState->hardwarePgaGainDb.load(std::memory_order_relaxed))) {
            m_sharedState->hardwarePgaGainDb.store(m_hwController.GetCapabilities().inputPgaCurrentDb,
                                                   std::memory_order_relaxed);
        }
        const float dacDb  = m_sharedState->hardwareDacGainDb.load(std::memory_order_relaxed);
        const float dacBal = m_sharedState->hardwareDacBalanceDb.load(std::memory_order_relaxed);
        if (m_hwController.SetOutputDacGainAndBalanceDb(dacDb, dacBal)) {
            m_sharedState->hardwareDacGainDb.store(m_hwController.GetCapabilities().outputDacCurrentDb,
                                                   std::memory_order_relaxed);
        }
    }

    // Opt-in Hardware Clip-Guard Auto-Pad: steps Node[6] PGA -2 dB on sustained clipping
    if (m_sharedState->autoPadOnClipEnabled.load(std::memory_order_relaxed) &&
        m_sharedState->adcClipDetected.load(std::memory_order_relaxed)) {
        const ULONGLONG nowMs = GetTickCount64();
        ++m_adcClipConfirmStreak;
        if (m_adcClipConfirmStreak >= 2 && (nowMs - m_lastAutoPadTickMs) >= 1500ULL) {
            const auto& c = m_hwController.GetCapabilities();
            if (c.hasInputPgaGain && c.inputPgaCurrentDb > c.inputPgaMinDb + 0.5f) {
                const float nextPga = std::max(c.inputPgaMinDb, c.inputPgaCurrentDb - 2.0f);
                if (m_hwController.SetInputPgaGainDb(nextPga)) {
                    m_sharedState->hardwarePgaGainDb.store(m_hwController.GetCapabilities().inputPgaCurrentDb,
                                                           std::memory_order_relaxed);
                    m_sharedState->adcClipDetected.store(false, std::memory_order_relaxed);
                    m_lastAutoPadTickMs = nowMs;
                }
            }
            m_adcClipConfirmStreak = 0;
        }
    } else {
        m_adcClipConfirmStreak = 0;
    }
}

bool WdmKsStream::CreateAudioPin(ULONG pinId, bool isCapture, uint32_t sampleRate, uint16_t bits,
                                 uint16_t channels, wil::unique_handle& outPin, uint16_t& outActualChannels) {
    outPin.reset();
    outActualChannels = channels;
    if (!m_filterHandle || !m_ksCreatePin) return false;

    const ACCESS_MASK access = isCapture ? GENERIC_READ : GENERIC_WRITE;

    struct KsDataFormatWaveFormatExtensible {
        KSDATAFORMAT         DataFormat;
        WAVEFORMATEXTENSIBLE WaveFormatExt;
    };

    auto tryCreate = [&](bool useExtensible, uint16_t tryBits, uint16_t tryCh) -> bool {
        const size_t fmtStructSize = useExtensible
            ? sizeof(KsDataFormatWaveFormatExtensible)
            : sizeof(KSDATAFORMAT_WAVEFORMATEX);
        std::vector<BYTE> mem(sizeof(KSPIN_CONNECT) + fmtStructSize, 0);

        auto* conn = reinterpret_cast<KSPIN_CONNECT*>(mem.data());
        conn->Interface.Set         = KSINTERFACESETID_Standard;
        conn->Interface.Id          = KSINTERFACE_STANDARD_STREAMING;
        conn->Interface.Flags       = 0;
        conn->Medium.Set            = KSMEDIUMSETID_Standard;
        conn->Medium.Id             = KSMEDIUM_TYPE_ANYINSTANCE;
        conn->Medium.Flags          = 0;
        conn->PinId                 = pinId;
        conn->PinToHandle           = nullptr;
        conn->Priority.PriorityClass    = KSPRIORITY_NORMAL;
        conn->Priority.PrioritySubClass = KSPRIORITY_NORMAL;

        const WORD blockAlign = static_cast<WORD>(tryCh * (tryBits / 8));
        if (useExtensible) {
            auto* df = reinterpret_cast<KsDataFormatWaveFormatExtensible*>(mem.data() + sizeof(KSPIN_CONNECT));
            df->DataFormat.FormatSize = sizeof(KsDataFormatWaveFormatExtensible);
            df->DataFormat.SampleSize = blockAlign;
            df->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
            df->DataFormat.SubFormat   = KSDATAFORMAT_SUBTYPE_PCM;
            df->DataFormat.Specifier   = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
            df->WaveFormatExt.Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
            df->WaveFormatExt.Format.nChannels            = tryCh;
            df->WaveFormatExt.Format.nSamplesPerSec       = sampleRate;
            df->WaveFormatExt.Format.wBitsPerSample       = tryBits;
            df->WaveFormatExt.Format.nBlockAlign          = blockAlign;
            df->WaveFormatExt.Format.nAvgBytesPerSec      = sampleRate * blockAlign;
            df->WaveFormatExt.Format.cbSize               = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            df->WaveFormatExt.Samples.wValidBitsPerSample = tryBits;
            df->WaveFormatExt.dwChannelMask               = (tryCh == 1) ? KSAUDIO_SPEAKER_MONO : KSAUDIO_SPEAKER_STEREO;
            df->WaveFormatExt.SubFormat                   = KSDATAFORMAT_SUBTYPE_PCM;
        } else {
            auto* df = reinterpret_cast<KSDATAFORMAT_WAVEFORMATEX*>(mem.data() + sizeof(KSPIN_CONNECT));
            df->DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEX);
            df->DataFormat.SampleSize = blockAlign;
            df->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
            df->DataFormat.SubFormat   = KSDATAFORMAT_SUBTYPE_PCM;
            df->DataFormat.Specifier   = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
            df->WaveFormatEx.wFormatTag      = WAVE_FORMAT_PCM;
            df->WaveFormatEx.nChannels       = tryCh;
            df->WaveFormatEx.nSamplesPerSec  = sampleRate;
            df->WaveFormatEx.wBitsPerSample  = tryBits;
            df->WaveFormatEx.nBlockAlign     = blockAlign;
            df->WaveFormatEx.nAvgBytesPerSec = sampleRate * blockAlign;
            df->WaveFormatEx.cbSize          = 0;
        }

        HANDLE hPin = INVALID_HANDLE_VALUE;
        const DWORD status = m_ksCreatePin(m_filterHandle.get(), conn, access, &hPin);
        m_lastPinStatus = status;
        if (status == ERROR_SUCCESS && hPin != INVALID_HANDLE_VALUE && hPin != nullptr) {
            outPin.reset(hPin);
            m_bitDepth        = tryBits;
            outActualChannels = tryCh;
            return true;
        }
        return false;
    };

    // If preferring 1ch mono capture, attempt 1ch first, then fall back cleanly to 2ch
    if (channels == 1) {
        if (tryCreate(false, bits, 1) || tryCreate(true, bits, 1)) return true;
    }
    if (tryCreate(false, bits, 2) || tryCreate(true, bits, 2)) return true;

    const uint16_t altBits = (bits == 24) ? 16 : 24;
    if (channels == 1 && (tryCreate(false, altBits, 1) || tryCreate(true, altBits, 1))) return true;
    if (tryCreate(false, altBits, 2) || tryCreate(true, altBits, 2)) return true;
    return false;
}

bool WdmKsStream::SetPinState(HANDLE hPin, KSSTATE state) {
    if (!hPin) return false;
    DWORD bytes = 0;
    return KsPropertyOverlapped(hPin, KSPROPSETID_Connection, KSPROPERTY_CONNECTION_STATE,
                                KSPROPERTY_TYPE_SET, nullptr, 0, &state, sizeof(state), &bytes);
}

bool WdmKsStream::CreateBuffers(uint32_t bufferSize,
                                uint32_t /*activeInputMask*/,
                                uint32_t /*activeOutputMask*/,
                                ASIOCallbacks* callbacks) {
    DisposeBuffers();

    m_bufferSize = std::clamp(bufferSize, 32u, 1024u);
    m_callbacks  = callbacks;
    m_activeIrpCount = m_sharedState
        ? std::clamp(m_sharedState->ksIrpCount.load(std::memory_order_relaxed), 2u, kMaxRenderIrps)
        : 3u;

    const uint32_t rate = static_cast<uint32_t>(m_sampleRate);
    uint16_t renCh = 2;
    if (!CreateAudioPin(m_renderPinId, false, rate, m_bitDepth, 2, m_renderPin, renCh)) {
        return false;
    }

    // Prefer native 1ch mono capture on Pin 1 if hardware supports it (halves USB IN bandwidth)
    const uint16_t desiredCapCh = m_hwController.GetCapabilities().captureSupportsMono ? 1 : 2;
    bool capOk = false;
    if (m_capturePinId != UINT32_MAX) {
        capOk = CreateAudioPin(m_capturePinId, true, rate, m_bitDepth, desiredCapCh, m_capturePin, m_capturePinChannels);
    }

    m_renderBytesPerBuffer  = m_bufferSize * 2 * (m_bitDepth / 8);
    m_captureBytesPerBuffer = m_bufferSize * m_capturePinChannels * (m_bitDepth / 8);
    m_stopEvent.create(wil::EventOptions::ManualReset);
    if (!m_stopEvent.is_valid()) {
        return false;
    }

    // Pre-allocate all kMaxRenderIrps (4) so Dynamic Zero-Crackle Autopilot can expand 2->3->4 without heap allocs
    m_renderIrps.resize(kMaxRenderIrps);
    const uint32_t capIrpCount = std::max<uint32_t>(
        m_activeIrpCount + 2u,
        static_cast<uint32_t>(std::ceil((m_sampleRate * 0.014) / static_cast<double>(m_bufferSize))));
    m_captureIrps.resize(capOk ? capIrpCount : 0);

    for (auto& slot : m_renderIrps) {
        slot.evt.create(wil::EventOptions::ManualReset);
        if (!slot.evt.is_valid()) return false;
        slot.pcm.assign(m_renderBytesPerBuffer, 0);
        slot.pending = false;
        m_ramLock.Lock(slot.pcm.data(), slot.pcm.size());
    }
    for (auto& slot : m_captureIrps) {
        slot.evt.create(wil::EventOptions::ManualReset);
        if (!slot.evt.is_valid()) return false;
        slot.pcm.assign(m_captureBytesPerBuffer, 0);
        slot.pending = false;
        m_ramLock.Lock(slot.pcm.data(), slot.pcm.size());
    }

    for (uint32_t ch = 0; ch < 2; ++ch) {
        m_asioIn[ch][0].assign(m_bufferSize, 0.0f);
        m_asioIn[ch][1].assign(m_bufferSize, 0.0f);
        m_asioOut[ch][0].assign(m_bufferSize, 0.0f);
        m_asioOut[ch][1].assign(m_bufferSize, 0.0f);
        m_workIn[ch].assign(m_bufferSize * 2, 0.0f);
        m_workOut[ch].assign(m_bufferSize * 2, 0.0f);

        m_ramLock.Lock(m_asioIn[ch][0].data(), m_asioIn[ch][0].size() * sizeof(float));
        m_ramLock.Lock(m_asioIn[ch][1].data(), m_asioIn[ch][1].size() * sizeof(float));
        m_ramLock.Lock(m_asioOut[ch][0].data(), m_asioOut[ch][0].size() * sizeof(float));
        m_ramLock.Lock(m_asioOut[ch][1].data(), m_asioOut[ch][1].size() * sizeof(float));
    }

    m_captureRingL.Resize(std::max<size_t>(8192, static_cast<size_t>(m_bufferSize) * 16));
    m_captureRingR.Resize(std::max<size_t>(8192, static_cast<size_t>(m_bufferSize) * 16));
    m_inputDsp.Prepare(m_sampleRate, 2);
    m_controlRoom.Prepare(m_sampleRate);
    m_dacConditioner.Prepare(m_sampleRate);
    m_captureAsrc.Prepare(m_sampleRate, m_bufferSize);
    m_captureAsrc.SetTargetFill(static_cast<double>(std::max<size_t>(
        static_cast<size_t>(m_bufferSize) * 2,
        static_cast<size_t>(m_sampleRate * 0.010))));

    m_inputLatencySamples  = m_bufferSize;
    m_outputLatencySamples = m_bufferSize * m_activeIrpCount;

    if (m_sharedState) {
        m_sharedState->captureActive.store(capOk, std::memory_order_relaxed);
        m_sharedState->captureMonoActive.store(capOk && (m_capturePinChannels == 1), std::memory_order_relaxed);
        m_sharedState->activeIrpCount.store(m_activeIrpCount, std::memory_order_relaxed);
        m_sharedState->activeBufferSize.store(m_bufferSize, std::memory_order_relaxed);
        m_sharedState->activeBitDepth.store(m_bitDepth, std::memory_order_relaxed);
        const float inMs  = static_cast<float>((m_inputLatencySamples * 1000.0) / m_sampleRate);
        const float outMs = static_cast<float>((m_outputLatencySamples * 1000.0) / m_sampleRate);
        m_sharedState->inputLatencyMs.store(inMs, std::memory_order_relaxed);
        m_sharedState->outputLatencyMs.store(outMs, std::memory_order_relaxed);
        m_sharedState->roundTripLatencyMs.store(inMs + outMs, std::memory_order_relaxed);
    }

    return true;
}

void WdmKsStream::DisposeBuffers() {
    Stop();
    m_ramLock.UnlockAll();
    m_capturePin.reset();
    m_renderPin.reset();
    m_renderIrps.clear();
    m_captureIrps.clear();
}

void* WdmKsStream::GetAsioBufferPtr(bool isInput, uint32_t channel, uint32_t doubleBufferIndex) noexcept {
    if (channel >= 2 || doubleBufferIndex >= 2) return nullptr;
    return isInput ? m_asioIn[channel][doubleBufferIndex].data()
                   : m_asioOut[channel][doubleBufferIndex].data();
}

void WdmKsStream::GetSamplePosition(uint64_t& outSamples, uint64_t& outSystemTimeNs) const noexcept {
    outSamples      = m_sampleCounter.load(std::memory_order_acquire);
    outSystemTimeNs = m_lastTimestampNs.load(std::memory_order_acquire);
}

bool WdmKsStream::SubmitRenderIrp(IrpSlot& slot) {
    slot.evt.ResetEvent();
    slot.ov = {};
    slot.ov.hEvent = slot.evt.get();

    slot.header = {};
    slot.header.Size        = sizeof(KSSTREAM_HEADER);
    slot.header.FrameExtent = m_renderBytesPerBuffer;
    slot.header.DataUsed    = m_renderBytesPerBuffer;
    slot.header.Data        = slot.pcm.data();

    DWORD bytes = 0;
    const BOOL ok = DeviceIoControl(m_renderPin.get(), IOCTL_KS_WRITE_STREAM,
                                    nullptr, 0,
                                    &slot.header, sizeof(KSSTREAM_HEADER),
                                    &bytes, &slot.ov);
    if (!ok) {
        if (GetLastError() == ERROR_IO_PENDING) {
            slot.pending = true;
            return true;
        }
        slot.pending = false;
        return false;
    }
    slot.pending = true;
    slot.evt.SetEvent();
    return true;
}

bool WdmKsStream::SubmitCaptureIrp(IrpSlot& slot) {
    slot.evt.ResetEvent();
    slot.ov = {};
    slot.ov.hEvent = slot.evt.get();

    slot.header = {};
    slot.header.Size        = sizeof(KSSTREAM_HEADER);
    slot.header.FrameExtent = m_captureBytesPerBuffer;
    slot.header.DataUsed    = 0;
    slot.header.Data        = slot.pcm.data();

    DWORD bytes = 0;
    const BOOL ok = DeviceIoControl(m_capturePin.get(), IOCTL_KS_READ_STREAM,
                                    nullptr, 0,
                                    &slot.header, sizeof(KSSTREAM_HEADER),
                                    &bytes, &slot.ov);
    if (!ok) {
        if (GetLastError() == ERROR_IO_PENDING) {
            slot.pending = true;
            return true;
        }
        slot.pending = false;
        return false;
    }
    slot.pending = true;
    slot.evt.SetEvent();
    return true;
}

void WdmKsStream::CancelAndDrainPins() {
    if (m_renderPin) {
        SetPinState(m_renderPin.get(), KSSTATE_PAUSE);
        SetPinState(m_renderPin.get(), KSSTATE_STOP);
        CancelIoEx(m_renderPin.get(), nullptr);
        for (auto& slot : m_renderIrps) {
            if (slot.pending) {
                DWORD dummy = 0;
                GetOverlappedResult(m_renderPin.get(), &slot.ov, &dummy, TRUE);
                slot.pending = false;
            }
        }
    }
    if (m_capturePin) {
        SetPinState(m_capturePin.get(), KSSTATE_PAUSE);
        SetPinState(m_capturePin.get(), KSSTATE_STOP);
        CancelIoEx(m_capturePin.get(), nullptr);
        for (auto& slot : m_captureIrps) {
            if (slot.pending) {
                DWORD dummy = 0;
                GetOverlappedResult(m_capturePin.get(), &slot.ov, &dummy, TRUE);
                slot.pending = false;
            }
        }
    }
}

bool WdmKsStream::Start() {
    if (m_running.load(std::memory_order_acquire)) return true;
    if (!m_renderPin) return false;

    m_stopEvent.ResetEvent();
    m_sampleCounter.store(0, std::memory_order_relaxed);
    m_captureUnderruns.store(0, std::memory_order_relaxed);
    m_captureTrims.store(0, std::memory_order_relaxed);
    m_captureRingL.Clear();
    m_captureRingR.Clear();
    m_inputDsp.Reset();
    m_controlRoom.Reset();
    m_dacConditioner.Reset();
    m_captureAsrc.Reset();

    m_timerScope.Acquire();

    if (m_capturePin) {
        SetPinState(m_capturePin.get(), KSSTATE_ACQUIRE);
        SetPinState(m_capturePin.get(), KSSTATE_PAUSE);
    }
    if (!SetPinState(m_renderPin.get(), KSSTATE_ACQUIRE) ||
        !SetPinState(m_renderPin.get(), KSSTATE_PAUSE)) {
        m_timerScope.Release();
        return false;
    }

    for (auto& slot : m_captureIrps) {
        SubmitCaptureIrp(slot);
    }
    for (uint32_t i = 0; i < m_activeIrpCount; ++i) {
        auto& slot = m_renderIrps[i];
        std::memset(slot.pcm.data(), 0, slot.pcm.size());
        if (!SubmitRenderIrp(slot)) {
            CancelAndDrainPins();
            m_timerScope.Release();
            return false;
        }
    }

    if (m_capturePin) {
        SetPinState(m_capturePin.get(), KSSTATE_RUN);
    }
    if (!SetPinState(m_renderPin.get(), KSSTATE_RUN)) {
        CancelAndDrainPins();
        m_timerScope.Release();
        return false;
    }

    m_running.store(true, std::memory_order_release);
    if (m_sharedState) {
        m_sharedState->streamRunning.store(true, std::memory_order_relaxed);
        m_sharedState->maxJitterUs.store(0.0f, std::memory_order_relaxed);
    }

    m_audioThread = std::thread(&WdmKsStream::AudioThreadMain, this);
    return true;
}

void WdmKsStream::Stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    m_stopEvent.SetEvent();
    if (m_audioThread.joinable()) {
        m_audioThread.join();
    }

    CancelAndDrainPins();
    m_timerScope.Release();

    if (m_sharedState) {
        m_sharedState->streamRunning.store(false, std::memory_order_relaxed);
    }
}

void WdmKsStream::AudioThreadMain() {
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcssHandle) {
        AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_CRITICAL);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    const bool pCore = m_sharedState ? m_sharedState->pCorePriority.load(std::memory_order_relaxed) : true;
    ApplyOptimalAudioThreadPriority(pCore);
    InputProcessor::EnableDenormalProtection();

    uint32_t rIdx = 0;
    uint32_t cIdx = 0;
    uint32_t doubleBufIdx = 0;
    uint32_t warmupBlocks = 0;
    uint32_t consecutiveXRuns = 0;
    uint32_t highLoadStreak = 0;

    LARGE_INTEGER prevWakeQpc{};
    QueryPerformanceCounter(&prevWakeQpc);
    const double expectedPeriodUs = (1000000.0 * m_bufferSize) / m_sampleRate;
    const uint32_t capBytesPerFrame = m_capturePinChannels * (m_bitDepth / 8);

    bool capturePrimed = !m_capturePin;
    const size_t minCapturePrime = std::max<size_t>(
        static_cast<size_t>(m_bufferSize) * 2,
        static_cast<size_t>(m_sampleRate * 0.010));
    m_captureAsrc.SetTargetFill(static_cast<double>(minCapturePrime));
    const size_t maxCaptureCeiling = std::max<size_t>(
        static_cast<size_t>(m_bufferSize) * 8,
        static_cast<size_t>(m_sampleRate * 0.035));

    ScopedRealtimeThread rtGuard;

    while (m_running.load(std::memory_order_acquire)) {
        IrpSlot& rSlot = m_renderIrps[rIdx];
        HANDLE waitHandles[2] = { m_stopEvent.get(), rSlot.evt.get() };

        const DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, 1000);
        if (waitRes == WAIT_OBJECT_0) {
            break;
        }
        if (waitRes != WAIT_OBJECT_0 + 1) {
            continue;
        }

        DWORD rBytes = 0;
        if (!GetOverlappedResult(m_renderPin.get(), &rSlot.ov, &rBytes, FALSE)) {
            const DWORD err = GetLastError();
            if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_OPERATION_ABORTED) {
                if (m_callbacks && m_callbacks->asioMessage) {
                    m_callbacks->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
                }
                break;
            }
        }
        rSlot.pending = false;

        LARGE_INTEGER wakeQpc{};
        QueryPerformanceCounter(&wakeQpc);
        const uint64_t nowNs = (wakeQpc.QuadPart * 1000000000ULL) / m_qpcFreq;
        m_lastTimestampNs.store(nowNs, std::memory_order_release);

        const double elapsedUs = (static_cast<double>(wakeQpc.QuadPart - prevWakeQpc.QuadPart) * 1000000.0) / m_qpcFreq;
        prevWakeQpc = wakeQpc;
        const float jitterUs = (warmupBlocks > 2)
            ? static_cast<float>(std::fabs(elapsedUs - expectedPeriodUs))
            : 0.0f;

        // 1. Harvest ALL completed Capture IRPs (non-blocking) and re-submit immediately
        if (m_capturePin && !m_captureIrps.empty()) {
            for (uint32_t checked = 0; checked < m_captureIrps.size(); ++checked) {
                IrpSlot& cSlot = m_captureIrps[cIdx];
                if (!cSlot.pending || WaitForSingleObject(cSlot.evt.get(), 0) != WAIT_OBJECT_0) {
                    break;
                }
                DWORD cBytes = 0;
                if (GetOverlappedResult(m_capturePin.get(), &cSlot.ov, &cBytes, FALSE)) {
                    const uint32_t validBytes = (cSlot.header.DataUsed > 0) ? cSlot.header.DataUsed : cBytes;
                    const uint32_t frames = std::min<uint32_t>(validBytes / capBytesPerFrame, m_bufferSize);
                    if (frames > 0) {
                        ConvertPcmToFloat(cSlot.pcm.data(), m_workIn[0].data(), m_workIn[1].data(),
                                          frames, m_capturePinChannels, m_bitDepth, false);
                        m_captureRingL.Push(m_workIn[0].data(), frames);
                        m_captureRingR.Push(m_workIn[1].data(), frames);
                    }
                }
                cSlot.pending = false;
                SubmitCaptureIrp(cSlot);
                cIdx = (cIdx + 1) % static_cast<uint32_t>(m_captureIrps.size());
            }

            const size_t avail = m_captureRingL.AvailableToRead();
            if (!capturePrimed && avail >= minCapturePrime) {
                capturePrimed = true;
            }
            if (avail > maxCaptureCeiling) {
                const size_t dropCount = avail - minCapturePrime;
                m_captureRingL.Drop(dropCount);
                m_captureRingR.Drop(dropCount);
                if (warmupBlocks > 16) {
                    m_captureTrims.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        // 2. Pop / PI-ASRC Lock m_bufferSize frames for ASIO input (Async ADC <-> Adaptive DAC lock)
        float* inL = m_asioIn[0][doubleBufIdx].data();
        float* inR = m_asioIn[1][doubleBufIdx].data();
        const bool useAsrc = !m_sharedState || m_sharedState->driftAsrcEnabled.load(std::memory_order_relaxed);
        bool capUnderrun = false;
        if (capturePrimed) {
            m_captureAsrc.Process(
                m_captureRingL, m_captureRingR, inL, inR, m_bufferSize,
                m_capturePinChannels == 1, !useAsrc, capUnderrun);
        } else {
            std::fill_n(inL, m_bufferSize, 0.0f);
            std::fill_n(inR, m_bufferSize, 0.0f);
        }
        if (capUnderrun && capturePrimed && warmupBlocks > 16 && m_capturePin) {
            m_captureUnderruns.fetch_add(1, std::memory_order_relaxed);
        }

        // 3. Shared DSP + ASIO bufferSwitchTimeInfo + Direct Monitor dispatch
        const uint64_t samplePos = m_sampleCounter.load(std::memory_order_relaxed);
        const auto disp = ProcessAsioHostBlock(
            m_sharedState, m_inputDsp,
            inL, inR,
            m_asioOut[0][doubleBufIdx].data(), m_asioOut[1][doubleBufIdx].data(),
            m_workOut[0].data(), m_workOut[1].data(),
            m_bufferSize, m_sampleRate, samplePos, nowNs, doubleBufIdx, m_callbacks, &m_controlRoom);

        // 4. ITU-R BS.1770-4 True-Peak ISP Guard + TPDF Dither / 96kHz Noise Shaper -> Submit Render IRP
        const bool ispOn = !m_sharedState || m_sharedState->dacIspGuardEnabled.load(std::memory_order_relaxed);
        const bool nsOn  = !m_sharedState || m_sharedState->dacDitherShaperEnabled.load(std::memory_order_relaxed);
        DacTelemetry dacTel{};
        m_dacConditioner.ProcessAndConvertToPcm(
            disp.outL, disp.outR, rSlot.pcm.data(), m_bufferSize,
            2, m_bitDepth, false, ispOn, nsOn, dacTel);
        if (!SubmitRenderIrp(rSlot)) {
            if (m_callbacks && m_callbacks->asioMessage) {
                m_callbacks->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
            }
            break;
        }

        m_sampleCounter.fetch_add(m_bufferSize, std::memory_order_release);
        doubleBufIdx ^= 1u;
        rIdx = (rIdx + 1) % m_activeIrpCount;
        ++warmupBlocks;

        // 5. Real-time telemetry + Dynamic Zero-Crackle IRP Autopilot
        if (m_sharedState) {
            const float outPeak = ComputeStereoPeakAvx2(disp.outL, disp.outR, m_bufferSize);
            const float outDb   = 20.0f * std::log10(std::max(outPeak, 1e-5f));
            m_sharedState->outputPeakDb.store(outDb, std::memory_order_relaxed);
            m_sharedState->outputTruePeakDb.store(dacTel.outputTruePeakDb, std::memory_order_relaxed);
            m_sharedState->dacIspReductionDb.store(dacTel.ispReductionDb, std::memory_order_relaxed);
            m_sharedState->clockDriftPpm.store(useAsrc ? m_captureAsrc.GetDriftPpm() : 0.0f,
                                               std::memory_order_relaxed);

            LARGE_INTEGER endQpc{};
            QueryPerformanceCounter(&endQpc);
            const double workUs = (static_cast<double>(endQpc.QuadPart - wakeQpc.QuadPart) * 1000000.0) / m_qpcFreq;
            const float cpuPct  = static_cast<float>(std::min(100.0, (workUs / expectedPeriodUs) * 100.0));

            m_sharedState->callbackCpuPercent.store(cpuPct, std::memory_order_relaxed);
            m_sharedState->lastJitterUs.store(jitterUs, std::memory_order_relaxed);
            const float prevMax = m_sharedState->maxJitterUs.load(std::memory_order_relaxed);
            if (jitterUs > prevMax) {
                m_sharedState->maxJitterUs.store(jitterUs, std::memory_order_relaxed);
            }

            const uint32_t idx = m_sharedState->jitterWriteIndex.fetch_add(1, std::memory_order_relaxed) % kJitterHistorySize;
            m_sharedState->jitterHistoryUs[idx].store(jitterUs, std::memory_order_relaxed);

            // Dynamic Zero-Crackle IRP Autopilot: expand proactively before XRun on sustained >=70% load
            // or immediately on any single >90% spike when running ultra-tight (2x IRPs).
            const bool needEmergencyExpand = (m_activeIrpCount == 2) && (workUs > expectedPeriodUs * 0.90);
            if (warmupBlocks > 4 && (workUs > expectedPeriodUs * 0.70 || needEmergencyExpand)) {
                ++highLoadStreak;
                if ((highLoadStreak >= 2 || needEmergencyExpand) &&
                    m_activeIrpCount < kMaxRenderIrps &&
                    m_sharedState->autopilotEnabled.load(std::memory_order_relaxed)) {
                    IrpSlot& extraSlot = m_renderIrps[m_activeIrpCount];
                    std::memset(extraSlot.pcm.data(), 0, extraSlot.pcm.size());
                    if (SubmitRenderIrp(extraSlot)) {
                        ++m_activeIrpCount;
                        m_sharedState->activeIrpCount.store(m_activeIrpCount, std::memory_order_relaxed);
                        const float newOutMs = static_cast<float>((m_bufferSize * m_activeIrpCount * 1000.0) / m_sampleRate);
                        m_sharedState->outputLatencyMs.store(newOutMs, std::memory_order_relaxed);
                        m_sharedState->roundTripLatencyMs.store(
                            m_sharedState->inputLatencyMs.load(std::memory_order_relaxed) + newOutMs,
                            std::memory_order_relaxed);
                    }
                    highLoadStreak = 0;
                }
            } else {
                highLoadStreak = 0;
            }

            if (warmupBlocks > 4 && workUs > expectedPeriodUs * (m_activeIrpCount - 1)) {
                m_sharedState->xrunCount.fetch_add(1, std::memory_order_relaxed);
                ++consecutiveXRuns;
                if (consecutiveXRuns >= 3 && m_sharedState->adaptiveXRunGuard.load(std::memory_order_relaxed)) {
                    if (m_callbacks && m_callbacks->asioMessage) {
                        m_callbacks->asioMessage(kAsioOverload, 0, nullptr, nullptr);
                    }
                    consecutiveXRuns = 0;
                }
            } else {
                consecutiveXRuns = 0;
            }
        }
    }

    if (mmcssHandle) {
        AvRevertMmThreadCharacteristics(mmcssHandle);
    }
}

} // namespace stoneridge
