#include "asio/StoneridgeASIO.h"
#include "ipc/ConfigPersistence.h"

#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cstring>

extern HMODULE g_hModule;

namespace stoneridge {

StoneridgeASIO::StoneridgeASIO() {
    OpenSharedMemory();
}

StoneridgeASIO::~StoneridgeASIO() {
    m_watcherRunning.store(false, std::memory_order_release);
    if (m_configWatcher.joinable()) {
        m_configWatcher.join();
    }
    disposeBuffers();
    if (m_stream) {
        m_stream->ShutdownDevices();
        m_stream.reset();
    }
    CloseSharedMemory();
}

void StoneridgeASIO::OpenSharedMemory() {
    m_shmHandle.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                         0, sizeof(SharedState), kSharedMemoryName));
    if (m_shmHandle) {
        const bool alreadyExists = (GetLastError() == ERROR_ALREADY_EXISTS);
        m_sharedState = static_cast<SharedState*>(
            MapViewOfFile(m_shmHandle.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
        if (m_sharedState && !alreadyExists) {
            InitializeDefaultSharedState(*m_sharedState);
            LoadConfigToSharedState(m_sharedState);
        }
    }
}

void StoneridgeASIO::CloseSharedMemory() {
    if (m_sharedState) {
        UnmapViewOfFile(m_sharedState);
        m_sharedState = nullptr;
    }
    m_shmHandle.reset();
}

void StoneridgeASIO::ConfigWatcherLoop() {
    uint32_t lastCounter = m_sharedState ? m_sharedState->configChangeCounter.load(std::memory_order_relaxed) : 0;
    while (m_watcherRunning.load(std::memory_order_acquire)) {
        Sleep(80);
        if (!m_sharedState) continue;

        if (m_stream) {
            m_stream->SyncHardwareControls();
        }

        if (!m_callbacks || !m_callbacks->asioMessage) {
            continue;
        }
        const uint32_t current = m_sharedState->configChangeCounter.load(std::memory_order_acquire);
        if (current != lastCounter) {
            lastCounter = current;
            m_callbacks->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
        }
    }
}

bool StoneridgeASIO::SelectAndInitializeBackend(uint32_t sampleRate, uint32_t bufferSize) {
    if (m_stream) {
        m_stream->Stop();
        m_stream->DisposeBuffers();
        m_stream->ShutdownDevices();
        m_stream.reset();
    }

    const auto desiredMode = m_sharedState
        ? static_cast<EngineMode>(m_sharedState->engineMode.load(std::memory_order_relaxed))
        : EngineMode::WdmKsDirect;

    if (desiredMode == EngineMode::WdmKsDirect) {
        auto ks = std::make_unique<WdmKsStream>();
        if (ks->InitializeDevices(sampleRate, bufferSize, m_sharedState)) {
            m_stream = std::move(ks);
            return true;
        }
    }

    // Fallback or explicitly requested WASAPI Exclusive Mode
    auto wasapi = std::make_unique<WasapiExclusiveStream>();
    if (wasapi->InitializeDevices(sampleRate, bufferSize, m_sharedState)) {
        m_stream = std::move(wasapi);
        return true;
    }

    return false;
}

HRESULT STDMETHODCALLTYPE StoneridgeASIO::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == IID_IUnknown || riid == CLSID_StoneridgeASIO) {
        *ppvObject = static_cast<IASIO*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE StoneridgeASIO::AddRef() {
    return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE StoneridgeASIO::Release() {
    const ULONG count = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (count == 0) {
        delete this;
    }
    return count;
}

ASIOBool StoneridgeASIO::init(void* sysHandle) {
    m_sysHandle = sysHandle;
    const uint32_t reqRate = m_sharedState
        ? static_cast<uint32_t>(m_sharedState->requestedSampleRate.load(std::memory_order_relaxed))
        : 44100;
    const uint32_t reqBuf = m_sharedState
        ? m_sharedState->requestedBufferSize.load(std::memory_order_relaxed)
        : 128;

    if (!SelectAndInitializeBackend(reqRate, reqBuf)) {
        std::snprintf(m_errorMsg, sizeof(m_errorMsg), "%s", "Failed to initialize Mackie EM-91CU+ audio engine.");
        return ASIOFalse;
    }

    m_initialized = true;
    if (!m_watcherRunning.exchange(true, std::memory_order_acq_rel)) {
        m_configWatcher = std::thread(&StoneridgeASIO::ConfigWatcherLoop, this);
    }
    return ASIOTrue;
}

void StoneridgeASIO::getDriverName(char* name) {
    if (name) {
        std::snprintf(name, 32, "%s", kDriverNameA);
    }
}

long StoneridgeASIO::getDriverVersion() {
    return 200;
}

void StoneridgeASIO::getErrorMessage(char* string) {
    if (string) {
        std::snprintf(string, 128, "%s", m_errorMsg);
    }
}

ASIOError StoneridgeASIO::start() {
    if (!m_initialized || !m_stream) return ASE_NotPresent;
    return m_stream->Start() ? ASE_OK : ASE_HWMalfunction;
}

ASIOError StoneridgeASIO::stop() {
    if (m_stream) m_stream->Stop();
    return ASE_OK;
}

ASIOError StoneridgeASIO::getChannels(long* numInputChannels, long* numOutputChannels) {
    if (!m_initialized || !m_stream) return ASE_NotPresent;
    if (numInputChannels)  *numInputChannels  = static_cast<long>(m_stream->GetInputChannels());
    if (numOutputChannels) *numOutputChannels = static_cast<long>(m_stream->GetOutputChannels());
    return ASE_OK;
}

ASIOError StoneridgeASIO::getLatencies(long* inputLatency, long* outputLatency) {
    if (!m_initialized || !m_stream) return ASE_NotPresent;
    if (inputLatency)  *inputLatency  = static_cast<long>(m_stream->GetInputLatencySamples());
    if (outputLatency) *outputLatency = static_cast<long>(m_stream->GetOutputLatencySamples());
    return ASE_OK;
}

ASIOError StoneridgeASIO::getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) {
    if (!m_stream) return ASE_NotPresent;
    if (minSize)       *minSize       = static_cast<long>(m_stream->GetMinBufferSize());
    if (maxSize)       *maxSize       = static_cast<long>(m_stream->GetMaxBufferSize());
    if (preferredSize) *preferredSize = static_cast<long>(m_stream->GetPreferredBufferSize());
    if (granularity)   *granularity   = m_stream->GetBufferGranularity();
    return ASE_OK;
}

ASIOError StoneridgeASIO::canSampleRate(ASIOSampleRate sampleRate) {
    if (!m_initialized || !m_stream) return ASE_NotPresent;
    const uint32_t mask = m_stream->GetSupportedSampleRatesMask();
    constexpr double kRates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
    for (uint32_t i = 0; i < 5; ++i) {
        if (std::abs(sampleRate - kRates[i]) < 1.0 && (mask & (1u << i))) {
            return ASE_OK;
        }
    }
    return ASE_NoClock;
}

ASIOError StoneridgeASIO::getSampleRate(ASIOSampleRate* sampleRate) {
    if (!sampleRate || !m_stream) return ASE_InvalidParameter;
    *sampleRate = m_stream->GetSampleRate();
    return ASE_OK;
}

ASIOError StoneridgeASIO::setSampleRate(ASIOSampleRate sampleRate) {
    if (canSampleRate(sampleRate) != ASE_OK) return ASE_NoClock;
    if (m_sharedState) {
        m_sharedState->requestedSampleRate.store(sampleRate, std::memory_order_relaxed);
        SaveSharedStateToConfig(m_sharedState);
    }
    const uint32_t buf = m_sharedState
        ? m_sharedState->requestedBufferSize.load(std::memory_order_relaxed)
        : (m_stream ? m_stream->GetBufferSize() : 128u);

    if (!SelectAndInitializeBackend(static_cast<uint32_t>(sampleRate), buf)) {
        return ASE_HWMalfunction;
    }
    if (m_callbacks && m_callbacks->sampleRateDidChange) {
        m_callbacks->sampleRateDidChange(sampleRate);
    }
    return ASE_OK;
}

ASIOError StoneridgeASIO::getClockSources(ASIOClockSource* clocks, long* numSources) {
    if (!clocks || !numSources || *numSources < 1) return ASE_InvalidParameter;
    clocks[0].index             = 0;
    clocks[0].associatedChannel = -1;
    clocks[0].associatedGroup   = -1;
    clocks[0].isCurrentSource   = ASIOTrue;
    std::snprintf(clocks[0].name, sizeof(clocks[0].name), "%s", "EM-91CU+ USB Crystal");
    *numSources = 1;
    return ASE_OK;
}

ASIOError StoneridgeASIO::setClockSource(long reference) {
    return (reference == 0) ? ASE_OK : ASE_InvalidParameter;
}

ASIOError StoneridgeASIO::getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) {
    uint64_t samples = 0, ns = 0;
    if (m_stream) {
        m_stream->GetSamplePosition(samples, ns);
    }
    if (sPos) {
        sPos->hi = static_cast<uint32_t>(samples >> 32);
        sPos->lo = static_cast<uint32_t>(samples & 0xFFFFFFFF);
    }
    if (tStamp) {
        tStamp->hi = static_cast<uint32_t>(ns >> 32);
        tStamp->lo = static_cast<uint32_t>(ns & 0xFFFFFFFF);
    }
    return ASE_OK;
}

ASIOError StoneridgeASIO::getChannelInfo(ASIOChannelInfo* info) {
    if (!info || !m_stream) return ASE_InvalidParameter;
    info->isActive     = ASIOTrue;
    info->channelGroup = 0;
    info->type         = ASIOSTFloat32LSB;

    if (info->isInput) {
        if (info->channel < 0 || info->channel >= static_cast<long>(m_stream->GetInputChannels())) {
            return ASE_InvalidParameter;
        }
        std::snprintf(info->name, sizeof(info->name), "EM-91CU+ Mic %ld", info->channel + 1);
    } else {
        if (info->channel < 0 || info->channel >= static_cast<long>(m_stream->GetOutputChannels())) {
            return ASE_InvalidParameter;
        }
        std::snprintf(info->name, sizeof(info->name), "EM-91CU+ Phones %ld", info->channel + 1);
    }
    return ASE_OK;
}

ASIOError StoneridgeASIO::createBuffers(ASIOBufferInfo* bufferInfos, long numChannels,
                                        long bufferSize, ASIOCallbacks* callbacks) {
    if (!m_initialized || !m_stream) return ASE_NotPresent;
    if (!bufferInfos || !callbacks || bufferSize <= 0) return ASE_InvalidParameter;

    m_callbacks = callbacks;
    uint32_t inMask = 0, outMask = 0;
    for (long i = 0; i < numChannels; ++i) {
        if (bufferInfos[i].isInput) {
            inMask |= (1u << bufferInfos[i].channelNum);
        } else {
            outMask |= (1u << bufferInfos[i].channelNum);
        }
    }

    if (!m_stream->CreateBuffers(static_cast<uint32_t>(bufferSize), inMask, outMask, callbacks)) {
        // If WDM-KS pin creation failed (e.g. another app owns KS pin exclusively), try WASAPI Exclusive fallback
        const auto activeMode = m_sharedState
            ? static_cast<EngineMode>(m_sharedState->activeEngineMode.load(std::memory_order_relaxed))
            : EngineMode::WdmKsDirect;
        if (activeMode == EngineMode::WdmKsDirect) {
            const uint32_t rate = static_cast<uint32_t>(m_stream->GetSampleRate());
            auto wasapi = std::make_unique<WasapiExclusiveStream>();
            if (wasapi->InitializeDevices(rate, static_cast<uint32_t>(bufferSize), m_sharedState) &&
                wasapi->CreateBuffers(static_cast<uint32_t>(bufferSize), inMask, outMask, callbacks)) {
                m_stream = std::move(wasapi);
            } else {
                std::snprintf(m_errorMsg, sizeof(m_errorMsg), "%s", "Failed to allocate WDM-KS or WASAPI Exclusive buffers.");
                return ASE_NoMemory;
            }
        } else {
            std::snprintf(m_errorMsg, sizeof(m_errorMsg), "%s", "Failed to allocate WASAPI Exclusive buffers.");
            return ASE_NoMemory;
        }
    }

    for (long i = 0; i < numChannels; ++i) {
        const bool isIn   = (bufferInfos[i].isInput != 0);
        const uint32_t ch = static_cast<uint32_t>(bufferInfos[i].channelNum);
        bufferInfos[i].buffers[0] = m_stream->GetAsioBufferPtr(isIn, ch, 0);
        bufferInfos[i].buffers[1] = m_stream->GetAsioBufferPtr(isIn, ch, 1);
    }

    return ASE_OK;
}

ASIOError StoneridgeASIO::disposeBuffers() {
    if (m_stream) m_stream->DisposeBuffers();
    m_callbacks = nullptr;
    return ASE_OK;
}

ASIOError StoneridgeASIO::controlPanel() {
    wchar_t dllPath[MAX_PATH]{};
    if (GetModuleFileNameW(g_hModule, dllPath, MAX_PATH) == 0) {
        return ASE_HWMalfunction;
    }
    PathRemoveFileSpecW(dllPath);
    std::wstring panelExe = std::wstring(dllPath) + L"\\StoneridgePanel.exe";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(panelExe.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, dllPath, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return ASE_OK;
    }
    return ASE_HWMalfunction;
}

ASIOError StoneridgeASIO::future(long selector, void* opt) {
    switch (selector) {
        case kAsioCanTimeInfo:
        case kAsioCanReportOverload:
            return ASE_SUCCESS;
        case kAsioGetInternalBufferSamples:
            if (opt && m_stream) {
                *static_cast<long*>(opt) = static_cast<long>(m_stream->GetBufferSize());
                return ASE_SUCCESS;
            }
            return ASE_InvalidParameter;
        default:
            return ASE_InvalidParameter;
    }
}

ASIOError StoneridgeASIO::outputReady() {
    return ASE_OK;
}

} // namespace stoneridge
