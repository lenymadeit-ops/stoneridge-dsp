#include "engine/WasapiExclusiveStream.h"
#include "engine/AsioHostBridge.h"
#include "engine/AudioFormatConvert.h"
#include "engine/OsRealtimeTuning.h"
#include "engine/RealtimeGuard.h"

#include <avrt.h>
#include <endpointvolume.h>
#include <ksmedia.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace stoneridge {

namespace {

HRESULT InitializeExclusiveClient(IMMDevice* device,
                                  const WAVEFORMATEXTENSIBLE& fmt,
                                  REFERENCE_TIME desiredPeriod,
                                  HANDLE eventHandle,
                                  wil::com_ptr<IAudioClient>& outClient,
                                  UINT32& outActualFrames) {
    if (!device) return E_POINTER;

    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(outClient.put()));
    if (FAILED(hr)) return hr;

    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    if (SUCCEEDED(outClient->GetDevicePeriod(&defPeriod, &minPeriod)) && minPeriod > 0) {
        desiredPeriod = std::max(desiredPeriod, minPeriod);
    }

    constexpr DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    hr = outClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags,
                               desiredPeriod, desiredPeriod,
                               reinterpret_cast<const WAVEFORMATEX*>(&fmt), nullptr);

    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 alignedFrames = 0;
        hr = outClient->GetBufferSize(&alignedFrames);
        if (FAILED(hr)) return hr;

        outClient.reset();
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(outClient.put()));
        if (FAILED(hr)) return hr;

        const REFERENCE_TIME alignedPeriod = static_cast<REFERENCE_TIME>(
            (10000000.0 * alignedFrames / fmt.Format.nSamplesPerSec) + 0.5);

        hr = outClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags,
                                   alignedPeriod, alignedPeriod,
                                   reinterpret_cast<const WAVEFORMATEX*>(&fmt), nullptr);
    }

    if (FAILED(hr)) return hr;

    hr = outClient->SetEventHandle(eventHandle);
    if (FAILED(hr)) return hr;

    hr = outClient->GetBufferSize(&outActualFrames);
    return hr;
}

uint32_t ProbeAlignedFrames(IMMDevice* device, const WAVEFORMATEXTENSIBLE& fmt, uint32_t requestedFrames, uint32_t& outMinFrames) {
    outMinFrames = requestedFrames;
    if (!device) return requestedFrames;

    wil::com_ptr<IAudioClient> c;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(c.put())))) {
        return requestedFrames;
    }

    REFERENCE_TIME defP = 0, minP = 0;
    if (SUCCEEDED(c->GetDevicePeriod(&defP, &minP)) && minP > 0) {
        outMinFrames = static_cast<uint32_t>(std::ceil((minP * fmt.Format.nSamplesPerSec) / 10000000.0));
    }

    REFERENCE_TIME reqP = static_cast<REFERENCE_TIME>((10000000.0 * requestedFrames / fmt.Format.nSamplesPerSec) + 0.5);
    if (minP > 0 && reqP < minP) reqP = minP;

    HRESULT hr = c->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                               reqP, reqP, reinterpret_cast<const WAVEFORMATEX*>(&fmt), nullptr);
    UINT32 actual = requestedFrames;
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED || SUCCEEDED(hr)) {
        if (SUCCEEDED(c->GetBufferSize(&actual)) && actual > 0) {
            return actual;
        }
    }
    return std::max(requestedFrames, outMinFrames);
}

} // namespace

WasapiExclusiveStream::WasapiExclusiveStream() {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    m_qpcFreq = static_cast<uint64_t>(freq.QuadPart);
}

WasapiExclusiveStream::~WasapiExclusiveStream() {
    Stop();
    DisposeBuffers();
    ShutdownDevices();
}

bool WasapiExclusiveStream::InitializeDevices(uint32_t sampleRate, uint32_t preferredBufferSize, SharedState* sharedState) {
    m_sharedState = sharedState;
    m_sampleRate  = static_cast<double>(sampleRate);
    m_bufferSize  = preferredBufferSize;

    m_deviceManager.SetDeviceChangeCallback([this]() {
        if (m_callbacks && m_callbacks->asioMessage) {
            m_callbacks->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
        }
    });
    m_deviceManager.StartMonitoring();

    std::wstring friendlyName;
    bool isMackie = false;
    if (!m_deviceManager.FindMackieEndpoints(m_captureDevice, m_renderDevice, friendlyName, isMackie)) {
        if (m_sharedState) {
            m_sharedState->deviceConnected.store(false, std::memory_order_relaxed);
        }
        return false;
    }

    if (m_sharedState) {
        m_sharedState->deviceConnected.store(true, std::memory_order_relaxed);
        m_sharedState->activeEngineMode.store(static_cast<uint32_t>(EngineMode::WasapiExclusive), std::memory_order_relaxed);
        const int written = WideCharToMultiByte(CP_UTF8, 0, friendlyName.c_str(), -1,
                                                m_sharedState->deviceName,
                                                static_cast<int>(sizeof(m_sharedState->deviceName)),
                                                nullptr, nullptr);
        if (written <= 0) {
            m_sharedState->deviceName[0] = '\0';
        }
        m_sharedState->deviceName[sizeof(m_sharedState->deviceName) - 1] = '\0';
    }

    wil::com_ptr<IAudioClient> probeClient;
    IMMDevice* probeDev = m_renderDevice ? m_renderDevice.get() : m_captureDevice.get();
    if (probeDev && SUCCEEDED(probeDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                 reinterpret_cast<void**>(probeClient.put())))) {
        m_supportedRatesMask = WasapiDeviceManager::ProbeSupportedSampleRates(probeClient.get());
        if (m_sharedState) {
            m_sharedState->supportedSampleRatesMask.store(m_supportedRatesMask, std::memory_order_relaxed);
        }
    }

    if (m_renderDevice) {
        wil::com_ptr<IAudioClient> tmp;
        if (SUCCEEDED(m_renderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                               reinterpret_cast<void**>(tmp.put())))) {
            if (!WasapiDeviceManager::ProbeBestExclusiveFormat(tmp.get(), sampleRate, m_renderFmt, m_bitDepth)) {
                return false;
            }
            m_outChannels = m_renderFmt.Format.nChannels;
        }
        // Probe exact hardware-aligned Exclusive frame count so ASIO runs 1:1 (ZERO double-callback bursts!)
        m_alignedPreferredFrames = ProbeAlignedFrames(m_renderDevice.get(), m_renderFmt, preferredBufferSize, m_minHardwareFrames);
    }

    if (m_captureDevice) {
        wil::com_ptr<IAudioClient> tmp;
        uint32_t capDepth = 24;
        if (SUCCEEDED(m_captureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                reinterpret_cast<void**>(tmp.put())))) {
            if (WasapiDeviceManager::ProbeBestExclusiveFormat(tmp.get(), sampleRate, m_captureFmt, capDepth)) {
                m_inChannels = m_captureFmt.Format.nChannels;
            } else {
                m_captureDevice.reset();
                m_inChannels = 0;
            }
        }
    }

    if (m_sharedState) {
        m_sharedState->activeSampleRate.store(m_sampleRate, std::memory_order_relaxed);
        m_sharedState->activeBitDepth.store(m_bitDepth, std::memory_order_relaxed);
        m_sharedState->inputChannels.store(m_inChannels, std::memory_order_relaxed);
        m_sharedState->outputChannels.store(m_outChannels, std::memory_order_relaxed);
    }

    return true;
}

void WasapiExclusiveStream::ShutdownDevices() {
    m_deviceManager.StopMonitoring();
    m_captureService.reset();
    m_renderService.reset();
    m_captureClient.reset();
    m_renderClient.reset();
    m_captureDevice.reset();
    m_renderDevice.reset();
}

bool WasapiExclusiveStream::CreateBuffers(uint32_t bufferSize,
                                          uint32_t activeInputMask,
                                          uint32_t activeOutputMask,
                                          ASIOCallbacks* callbacks) {
    DisposeBuffers();

    m_bufferSize       = bufferSize;
    m_activeInputMask  = activeInputMask;
    m_activeOutputMask = activeOutputMask;
    m_callbacks        = callbacks;

    m_renderEvent.create(wil::EventOptions::None);
    m_captureEvent.create(wil::EventOptions::None);
    m_stopEvent.create(wil::EventOptions::ManualReset);

    const REFERENCE_TIME desiredPeriod = static_cast<REFERENCE_TIME>(
        (10000000.0 * m_bufferSize / m_sampleRate) + 0.5);

    UINT32 actualRenderFrames = m_bufferSize;
    if (m_renderDevice) {
        const HRESULT hr = InitializeExclusiveClient(m_renderDevice.get(), m_renderFmt,
                                                     desiredPeriod, m_renderEvent.get(),
                                                     m_renderClient, actualRenderFrames);
        if (FAILED(hr)) return false;
        if (FAILED(m_renderClient->GetService(IID_PPV_ARGS(&m_renderService)))) return false;
        m_wasapiRenderFrames = actualRenderFrames;
    } else {
        m_wasapiRenderFrames = m_bufferSize;
    }

    bool capOk = false;
    if (m_captureDevice) {
        UINT32 actualCapFrames = m_bufferSize;
        HRESULT hr = InitializeExclusiveClient(m_captureDevice.get(), m_captureFmt,
                                               desiredPeriod, m_captureEvent.get(),
                                               m_captureClient, actualCapFrames);
        if (FAILED(hr)) {
            m_captureClient.reset();
            if (SUCCEEDED(m_captureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                    reinterpret_cast<void**>(m_captureClient.put())))) {
                const DWORD shFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
                if (SUCCEEDED(m_captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED, shFlags,
                                                          desiredPeriod, 0,
                                                          reinterpret_cast<const WAVEFORMATEX*>(&m_captureFmt), nullptr)) &&
                    SUCCEEDED(m_captureClient->SetEventHandle(m_captureEvent.get()))) {
                    hr = S_OK;
                }
            }
        }
        if (SUCCEEDED(hr) && m_captureClient) {
            capOk = SUCCEEDED(m_captureClient->GetService(IID_PPV_ARGS(&m_captureService)));
        }
        if (!capOk) {
            m_captureClient.reset();
            m_captureService.reset();
        }
    }

    REFERENCE_TIME capLat100ns = 0, renLat100ns = 0;
    if (m_captureClient) m_captureClient->GetStreamLatency(&capLat100ns);
    if (m_renderClient)  m_renderClient->GetStreamLatency(&renLat100ns);

    const uint32_t hwCapSamples = static_cast<uint32_t>((capLat100ns * m_sampleRate) / 10000000.0);
    const uint32_t hwRenSamples = static_cast<uint32_t>((renLat100ns * m_sampleRate) / 10000000.0);
    m_inputLatencySamples  = m_bufferSize + hwCapSamples;
    m_outputLatencySamples = m_wasapiRenderFrames + hwRenSamples;

    const uint32_t maxBlock = std::max(m_bufferSize, m_wasapiRenderFrames);
    for (uint32_t ch = 0; ch < 2; ++ch) {
        m_asioIn[ch][0].assign(m_bufferSize, 0.0f);
        m_asioIn[ch][1].assign(m_bufferSize, 0.0f);
        m_asioOut[ch][0].assign(m_bufferSize, 0.0f);
        m_asioOut[ch][1].assign(m_bufferSize, 0.0f);
        m_workIn[ch].assign(maxBlock * 2, 0.0f);
        m_workOut[ch].assign(maxBlock * 2, 0.0f);
    }

    m_captureRingL.Resize(std::max<size_t>(4096, maxBlock * 8));
    m_captureRingR.Resize(std::max<size_t>(4096, maxBlock * 8));
    m_renderRingL.Resize(std::max<size_t>(4096, maxBlock * 8));
    m_renderRingR.Resize(std::max<size_t>(4096, maxBlock * 8));
    m_inputDsp.Prepare(m_sampleRate, 2);
    m_controlRoom.Prepare(m_sampleRate);
    m_dacConditioner.Prepare(m_sampleRate);

    if (m_sharedState) {
        m_sharedState->captureActive.store(capOk, std::memory_order_relaxed);
        m_sharedState->activeBufferSize.store(m_bufferSize, std::memory_order_relaxed);
        const float inMs  = static_cast<float>((m_inputLatencySamples * 1000.0) / m_sampleRate);
        const float outMs = static_cast<float>((m_outputLatencySamples * 1000.0) / m_sampleRate);
        m_sharedState->inputLatencyMs.store(inMs, std::memory_order_relaxed);
        m_sharedState->outputLatencyMs.store(outMs, std::memory_order_relaxed);
        m_sharedState->roundTripLatencyMs.store(inMs + outMs, std::memory_order_relaxed);
    }

    return true;
}

void WasapiExclusiveStream::DisposeBuffers() {
    Stop();
    m_captureService.reset();
    m_renderService.reset();
    m_captureClient.reset();
    m_renderClient.reset();
}

void* WasapiExclusiveStream::GetAsioBufferPtr(bool isInput, uint32_t channel, uint32_t doubleBufferIndex) noexcept {
    if (channel >= 2 || doubleBufferIndex >= 2) return nullptr;
    return isInput ? m_asioIn[channel][doubleBufferIndex].data()
                   : m_asioOut[channel][doubleBufferIndex].data();
}

void WasapiExclusiveStream::GetSamplePosition(uint64_t& outSamples, uint64_t& outSystemTimeNs) const noexcept {
    outSamples      = m_sampleCounter.load(std::memory_order_acquire);
    outSystemTimeNs = m_lastTimestampNs.load(std::memory_order_acquire);
}

bool WasapiExclusiveStream::Start() {
    if (m_running.load(std::memory_order_acquire)) return true;
    if (!m_renderClient || !m_renderService) return false;

    m_stopEvent.ResetEvent();
    m_sampleCounter.store(0, std::memory_order_relaxed);
    m_captureRingL.Clear();
    m_captureRingR.Clear();
    m_renderRingL.Clear();
    m_renderRingR.Clear();
    m_inputDsp.Reset();
    m_controlRoom.Reset();
    m_dacConditioner.Reset();

    m_hiResTimer.Acquire();

    BYTE* primeData = nullptr;
    if (SUCCEEDED(m_renderService->GetBuffer(m_wasapiRenderFrames, &primeData))) {
        std::memset(primeData, 0, m_wasapiRenderFrames * m_renderFmt.Format.nBlockAlign);
        m_renderService->ReleaseBuffer(m_wasapiRenderFrames, 0);
    }

    if (m_captureClient) {
        m_captureClient->Start();
    }
    if (FAILED(m_renderClient->Start())) {
        if (m_captureClient) m_captureClient->Stop();
        m_hiResTimer.Release();
        return false;
    }

    m_running.store(true, std::memory_order_release);
    if (m_sharedState) {
        m_sharedState->streamRunning.store(true, std::memory_order_relaxed);
        m_sharedState->maxJitterUs.store(0.0f, std::memory_order_relaxed);
    }

    m_audioThread = std::thread(&WasapiExclusiveStream::AudioThreadMain, this);
    return true;
}

void WasapiExclusiveStream::Stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    m_stopEvent.SetEvent();
    if (m_audioThread.joinable()) {
        m_audioThread.join();
    }

    if (m_renderClient) m_renderClient->Stop();
    if (m_captureClient) m_captureClient->Stop();
    m_hiResTimer.Release();

    if (m_sharedState) {
        m_sharedState->streamRunning.store(false, std::memory_order_relaxed);
    }
}

void WasapiExclusiveStream::ApplyThreadOptimizations() {
    DisableWin11TimerThrottling();
    InputProcessor::EnableDenormalProtection();

    const bool pCore = m_sharedState ? m_sharedState->pCorePriority.load(std::memory_order_relaxed) : true;
    ApplyOptimalAudioThreadPriority(pCore);
}

void WasapiExclusiveStream::DrainCaptureExclusive() {
    if (!m_captureService) return;
    const bool isFloat = (m_captureFmt.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

    // Proper Exclusive-mode capture loop: call GetBuffer until AUDCLNT_S_BUFFER_EMPTY
    for (int iter = 0; iter < 8; ++iter) {
        BYTE* capData = nullptr;
        UINT32 framesRead = 0;
        DWORD capFlags = 0;
        const HRESULT hr = m_captureService->GetBuffer(&capData, &framesRead, &capFlags, nullptr, nullptr);
        if (hr == AUDCLNT_S_BUFFER_EMPTY || FAILED(hr)) {
            break;
        }
        if (framesRead > 0) {
            const uint32_t chunk = std::min<uint32_t>(framesRead, static_cast<uint32_t>(m_workIn[0].size()));
            if (capFlags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::fill_n(m_workIn[0].data(), chunk, 0.0f);
                std::fill_n(m_workIn[1].data(), chunk, 0.0f);
            } else {
                ConvertPcmToFloat(capData, m_workIn[0].data(), m_workIn[1].data(), chunk,
                                  m_captureFmt.Format.nChannels, m_captureFmt.Format.wBitsPerSample, isFloat);
            }
            m_captureRingL.Push(m_workIn[0].data(), chunk);
            m_captureRingR.Push(m_workIn[1].data(), chunk);
        }
        m_captureService->ReleaseBuffer(framesRead);
    }
}

void WasapiExclusiveStream::AudioThreadMain() {
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcssHandle) {
        AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_CRITICAL);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    ApplyThreadOptimizations();

    HANDLE waitHandles[3] = { m_stopEvent.get(), m_renderEvent.get(), m_captureEvent.get() };
    const DWORD numHandles = m_captureService ? 3u : 2u;
    uint32_t doubleBufIdx = 0;
    uint32_t warmupBlocks = 0;
    uint32_t consecutiveXRuns = 0;
    float lastCapL = 0.0f;
    float lastCapR = 0.0f;

    LARGE_INTEGER prevWakeQpc{};
    QueryPerformanceCounter(&prevWakeQpc);
    const double expectedPeriodUs = (1000000.0 * m_wasapiRenderFrames) / m_sampleRate;
    const bool renIsFloat = (m_renderFmt.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

    ScopedRealtimeThread rtGuard;

    while (m_running.load(std::memory_order_acquire)) {
        const DWORD waitRes = WaitForMultipleObjects(numHandles, waitHandles, FALSE, 1000);
        if (waitRes == WAIT_OBJECT_0) {
            break;
        }
        if (waitRes == WAIT_OBJECT_0 + 2) {
            // Capture event woke us independently -> drain immediately without touching Render!
            DrainCaptureExclusive();
            continue;
        }
        if (waitRes != WAIT_OBJECT_0 + 1) {
            continue;
        }

        LARGE_INTEGER wakeQpc{};
        QueryPerformanceCounter(&wakeQpc);
        const uint64_t nowNs = (wakeQpc.QuadPart * 1000000000ULL) / m_qpcFreq;
        m_lastTimestampNs.store(nowNs, std::memory_order_release);

        const double elapsedUs = (static_cast<double>(wakeQpc.QuadPart - prevWakeQpc.QuadPart) * 1000000.0) / m_qpcFreq;
        prevWakeQpc = wakeQpc;
        const float jitterUs = (warmupBlocks > 2)
            ? static_cast<float>(std::fabs(elapsedUs - expectedPeriodUs))
            : 0.0f;

        // 1. Drain any pending capture packets
        DrainCaptureExclusive();

        // Gentle drift safety only if capture exceeds 3x buffer
        const size_t capAvail = m_captureRingL.AvailableToRead();
        if (capAvail > static_cast<size_t>(std::max(m_bufferSize, m_wasapiRenderFrames)) * 3) {
            const size_t excess = capAvail - m_bufferSize;
            m_captureRingL.Drop(excess);
            m_captureRingR.Drop(excess);
        }

        // 2. Run FL Studio ASIO block(s) BEFORE acquiring WASAPI Render GetBuffer() lock!
        auto runAsioBlock = [&]() {
            float* inL = m_asioIn[0][doubleBufIdx].data();
            float* inR = m_asioIn[1][doubleBufIdx].data();
            const size_t poppedL = m_captureRingL.Pop(inL, m_bufferSize);
            const size_t poppedR = m_captureRingR.Pop(inR, m_bufferSize);

            if (poppedL > 0) lastCapL = inL[poppedL - 1];
            if (poppedR > 0) lastCapR = inR[poppedR - 1];
            for (size_t i = poppedL; i < m_bufferSize; ++i) {
                lastCapL *= 0.92f;
                inL[i] = lastCapL;
            }
            for (size_t i = poppedR; i < m_bufferSize; ++i) {
                lastCapR *= 0.92f;
                inR[i] = lastCapR;
            }

            const uint64_t samplePos = m_sampleCounter.load(std::memory_order_relaxed);
            const auto disp = ProcessAsioHostBlock(
                m_sharedState, m_inputDsp,
                inL, inR,
                m_asioOut[0][doubleBufIdx].data(), m_asioOut[1][doubleBufIdx].data(),
                m_workOut[0].data(), m_workOut[1].data(),
                m_bufferSize, m_sampleRate, samplePos, nowNs, doubleBufIdx, m_callbacks, &m_controlRoom);

            if (m_wasapiRenderFrames != m_bufferSize) {
                m_renderRingL.Push(disp.outL, m_bufferSize);
                m_renderRingR.Push(disp.outR, m_bufferSize);
            }

            m_sampleCounter.fetch_add(m_bufferSize, std::memory_order_release);
            doubleBufIdx ^= 1u;
        };

        const float* finalL = nullptr;
        const float* finalR = nullptr;

        if (m_wasapiRenderFrames == m_bufferSize) {
            const uint32_t usedIdx = doubleBufIdx;
            runAsioBlock();
            const bool hasMon = m_sharedState &&
                (m_sharedState->directMonitorMix.load(std::memory_order_relaxed) > 0.001f ||
                 m_sharedState->crossfeedEnabled.load(std::memory_order_relaxed));
            finalL = hasMon ? m_workOut[0].data() : m_asioOut[0][usedIdx].data();
            finalR = hasMon ? m_workOut[1].data() : m_asioOut[1][usedIdx].data();
        } else {
            runAsioBlock();
            const uint32_t have = static_cast<uint32_t>(
                std::min(m_renderRingL.AvailableToRead(), static_cast<size_t>(m_wasapiRenderFrames)));
            if (have > 0) {
                m_renderRingL.Pop(m_workOut[0].data(), have);
                m_renderRingR.Pop(m_workOut[1].data(), have);
            }
            for (uint32_t i = have; i < m_wasapiRenderFrames; ++i) {
                const uint32_t src = (have == 0) ? 0u : (have - 1u);
                m_workOut[0][i] = (have == 0) ? 0.0f : m_workOut[0][src];
                m_workOut[1][i] = (have == 0) ? 0.0f : m_workOut[1][src];
            }
            finalL = m_workOut[0].data();
            finalR = m_workOut[1].data();
        }

        // 3. Acquire WASAPI Render buffer ONLY for the fast convert (< 10 us!)
        BYTE* renderData = nullptr;
        const HRESULT hrGet = m_renderService->GetBuffer(m_wasapiRenderFrames, &renderData);
        if (SUCCEEDED(hrGet)) {
            const bool ispOn = !m_sharedState || m_sharedState->dacIspGuardEnabled.load(std::memory_order_relaxed);
            const bool nsOn  = !m_sharedState || m_sharedState->dacDitherShaperEnabled.load(std::memory_order_relaxed);
            DacTelemetry dacTel{};
            m_dacConditioner.ProcessAndConvertToPcm(
                finalL, finalR, renderData, m_wasapiRenderFrames,
                m_renderFmt.Format.nChannels, m_renderFmt.Format.wBitsPerSample, renIsFloat,
                ispOn, nsOn, dacTel);
            m_renderService->ReleaseBuffer(m_wasapiRenderFrames, 0);

            if (m_sharedState) {
                const float outPeak = ComputeStereoPeakAvx2(finalL, finalR, m_wasapiRenderFrames);
                const float outDb   = 20.0f * std::log10(std::max(outPeak, 1e-5f));
                m_sharedState->outputPeakDb.store(outDb, std::memory_order_relaxed);
                m_sharedState->outputTruePeakDb.store(dacTel.outputTruePeakDb, std::memory_order_relaxed);
                m_sharedState->dacIspReductionDb.store(dacTel.ispReductionDb, std::memory_order_relaxed);
            }
        } else if (hrGet == AUDCLNT_E_DEVICE_INVALIDATED) {
            if (m_callbacks && m_callbacks->asioMessage) {
                m_callbacks->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
            }
            break;
        }

        ++warmupBlocks;

        if (m_sharedState) {
            LARGE_INTEGER endQpc{};
            QueryPerformanceCounter(&endQpc);
            const double workUs = (static_cast<double>(endQpc.QuadPart - wakeQpc.QuadPart) * 1000000.0) / m_qpcFreq;
            const float cpuPct  = static_cast<float>(std::min(100.0, (workUs / expectedPeriodUs) * 100.0));

            m_sharedState->callbackCpuPercent.store(cpuPct, std::memory_order_relaxed);
            m_sharedState->lastJitterUs.store(jitterUs, std::memory_order_relaxed);
            float prevMax = m_sharedState->maxJitterUs.load(std::memory_order_relaxed);
            if (jitterUs > prevMax) {
                m_sharedState->maxJitterUs.store(jitterUs, std::memory_order_relaxed);
            }

            const uint32_t idx = m_sharedState->jitterWriteIndex.fetch_add(1, std::memory_order_relaxed) % kJitterHistorySize;
            m_sharedState->jitterHistoryUs[idx].store(jitterUs, std::memory_order_relaxed);

            if (warmupBlocks > 4 && workUs > expectedPeriodUs) {
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
    if (SUCCEEDED(hrCo)) {
        CoUninitialize();
    }
}

void WasapiExclusiveStream::SyncHardwareControls() {
    if (!m_sharedState || !m_renderDevice) return;
    IAudioEndpointVolume* vol = nullptr;
    if (FAILED(m_renderDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(&vol))) || !vol) {
        return;
    }
    const float db = std::clamp(m_sharedState->hardwareDacGainDb.load(std::memory_order_relaxed), -63.5f, 0.0f);
    const float scalar = std::pow(10.0f, db / 20.0f);
    vol->SetMasterVolumeLevelScalar(std::clamp(scalar, 0.0f, 1.0f), nullptr);
    vol->SetMute(m_sharedState->inputMute.load(std::memory_order_relaxed) ? TRUE : FALSE, nullptr);
    vol->Release();
}

} // namespace stoneridge
