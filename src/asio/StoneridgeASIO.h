#pragma once

#include "asio/asio_abi.h"
#include "engine/IAudioStreamBackend.h"
#include "engine/WasapiExclusiveStream.h"
#include "engine/WdmKsStream.h"
#include "ipc/SharedState.h"

#include <wil/resource.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace stoneridge {

// {B8E3A1D4-7C2F-4E9A-9D12-8F4E2C1A5B01}
inline constexpr GUID CLSID_StoneridgeASIO = {
    0xb8e3a1d4, 0x7c2f, 0x4e9a, { 0x9d, 0x12, 0x8f, 0x4e, 0x2c, 0x1a, 0x5b, 0x01 }
};

// Hosts that QueryInterface the Steinberg IASIO IID rather than the driver CLSID.
inline constexpr GUID IID_IASIO_Steinberg = {
    0x48d0c245, 0x0368, 0x11d1, { 0xa8, 0x4b, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 }
};

inline constexpr wchar_t kClsidString[]      = L"{B8E3A1D4-7C2F-4E9A-9D12-8F4E2C1A5B01}";
inline constexpr wchar_t kDriverNameW[]      = L"Mackie EM-91CU+ (Stoneridge)";
inline constexpr char    kDriverNameA[]      = "Mackie EM-91CU+ (Stoneridge)";

class StoneridgeASIO final : public IASIO {
public:
    StoneridgeASIO();
    ~StoneridgeASIO();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;

    // IASIO
    ASIOBool  init(void* sysHandle) override;
    void      getDriverName(char* name) override;
    long      getDriverVersion() override;
    void      getErrorMessage(char* string) override;
    ASIOError start() override;
    ASIOError stop() override;
    ASIOError getChannels(long* numInputChannels, long* numOutputChannels) override;
    ASIOError getLatencies(long* inputLatency, long* outputLatency) override;
    ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) override;
    ASIOError canSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getSampleRate(ASIOSampleRate* sampleRate) override;
    ASIOError setSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) override;
    ASIOError setClockSource(long reference) override;
    ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override;
    ASIOError getChannelInfo(ASIOChannelInfo* info) override;
    ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) override;
    ASIOError disposeBuffers() override;
    ASIOError controlPanel() override;
    ASIOError future(long selector, void* opt) override;
    ASIOError outputReady() override;

private:
    bool SelectAndInitializeBackend(uint32_t sampleRate, uint32_t bufferSize);
    void OpenSharedMemory();
    void CloseSharedMemory();
    void ConfigWatcherLoop();

    std::atomic<ULONG> m_refCount{1};
    void*              m_sysHandle = nullptr;
    bool               m_initialized = false;
    char               m_errorMsg[128] = {};

    std::unique_ptr<IAudioStreamBackend> m_stream;
    ASIOCallbacks*                       m_callbacks = nullptr;

    wil::unique_handle m_shmHandle;
    SharedState*       m_sharedState = nullptr;

    std::thread        m_configWatcher;
    std::atomic<bool>  m_watcherRunning{false};
};

} // namespace stoneridge
