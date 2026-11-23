#pragma once

#include "engine/IAudioStreamBackend.h"
#include "engine/OsRealtimeTuning.h"
#include "dsp/ControlRoomMonitor.h"
#include "dsp/DacConditioner.h"
#include "dsp/InputProcessor.h"
#include "engine/LockFreeRingBuffer.h"
#include "engine/WasapiDeviceManager.h"

#include <audioclient.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace stoneridge {

class WasapiExclusiveStream final : public IAudioStreamBackend {
public:
    WasapiExclusiveStream();
    ~WasapiExclusiveStream() override;

    bool InitializeDevices(uint32_t sampleRate, uint32_t preferredBufferSize, SharedState* sharedState) override;
    void ShutdownDevices() override;

    bool CreateBuffers(uint32_t bufferSize,
                       uint32_t activeInputMask,
                       uint32_t activeOutputMask,
                       ASIOCallbacks* callbacks) override;
    void DisposeBuffers() override;

    bool Start() override;
    void Stop() override;

    uint32_t GetBufferSize() const noexcept override { return m_bufferSize; }
    uint32_t GetPreferredBufferSize() const noexcept override { return m_alignedPreferredFrames; }
    uint32_t GetMinBufferSize() const noexcept override { return m_minHardwareFrames; }
    uint32_t GetMaxBufferSize() const noexcept override { return 1024; }
    long     GetBufferGranularity() const noexcept override { return 1; }

    double   GetSampleRate() const noexcept override { return m_sampleRate; }
    uint32_t GetInputChannels() const noexcept override { return m_inChannels; }
    uint32_t GetOutputChannels() const noexcept override { return m_outChannels; }
    uint32_t GetInputLatencySamples() const noexcept override { return m_inputLatencySamples; }
    uint32_t GetOutputLatencySamples() const noexcept override { return m_outputLatencySamples; }
    uint32_t GetSupportedSampleRatesMask() const noexcept override { return m_supportedRatesMask; }

    void* GetAsioBufferPtr(bool isInput, uint32_t channel, uint32_t doubleBufferIndex) noexcept override;
    void  GetSamplePosition(uint64_t& outSamples, uint64_t& outSystemTimeNs) const noexcept override;
    void  SyncHardwareControls() override;

private:
    void AudioThreadMain();
    void DrainCaptureExclusive();
    void ApplyThreadOptimizations();

    WasapiDeviceManager m_deviceManager;
    SharedState*        m_sharedState = nullptr;
    ASIOCallbacks*      m_callbacks   = nullptr;

    wil::com_ptr<IMMDevice>           m_captureDevice;
    wil::com_ptr<IMMDevice>           m_renderDevice;
    wil::com_ptr<IAudioClient>        m_captureClient;
    wil::com_ptr<IAudioClient>        m_renderClient;
    wil::com_ptr<IAudioCaptureClient> m_captureService;
    wil::com_ptr<IAudioRenderClient>  m_renderService;

    wil::unique_event m_renderEvent;
    wil::unique_event m_captureEvent;
    wil::unique_event m_stopEvent;

    WAVEFORMATEXTENSIBLE m_captureFmt{};
    WAVEFORMATEXTENSIBLE m_renderFmt{};

    double   m_sampleRate             = 48000.0;
    uint32_t m_bufferSize             = 144;
    uint32_t m_wasapiRenderFrames     = 144;
    uint32_t m_minHardwareFrames      = 144;
    uint32_t m_alignedPreferredFrames = 144;
    uint32_t m_inputLatencySamples    = 144;
    uint32_t m_outputLatencySamples   = 144;
    uint32_t m_inChannels             = 2;
    uint32_t m_outChannels            = 2;
    uint32_t m_bitDepth               = 24;
    uint32_t m_supportedRatesMask     = 0b01011;
    uint32_t m_activeInputMask        = 0;
    uint32_t m_activeOutputMask       = 0;

    std::vector<float> m_asioIn[2][2];
    std::vector<float> m_asioOut[2][2];
    std::vector<float> m_workIn[2];
    std::vector<float> m_workOut[2];
    LockFreeRingBuffer<float> m_captureRingL;
    LockFreeRingBuffer<float> m_captureRingR;
    LockFreeRingBuffer<float> m_renderRingL;
    LockFreeRingBuffer<float> m_renderRingR;

    ScopedHighResTimer m_hiResTimer;
    InputProcessor m_inputDsp;
    ControlRoomMonitor m_controlRoom;
    DacConditioner m_dacConditioner;

    std::thread           m_audioThread;
    std::atomic<bool>     m_running{false};
    std::atomic<uint64_t> m_sampleCounter{0};
    std::atomic<uint64_t> m_lastTimestampNs{0};

    uint64_t m_qpcFreq = 1;
};

} // namespace stoneridge
