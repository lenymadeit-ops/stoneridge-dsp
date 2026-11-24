#pragma once

#include "engine/IAudioStreamBackend.h"
#include "engine/AsioHostBridge.h"
#include "dsp/ControlRoomMonitor.h"
#include "dsp/DacConditioner.h"
#include "dsp/InputProcessor.h"
#include "engine/AdaptiveDriftResampler.h"
#include "engine/LockFreeRingBuffer.h"
#include "engine/MackieHidMonitor.h"
#include "engine/OsRealtimeTuning.h"
#include "engine/WdmKsHardwareProbe.h"

#include <windows.h>
#include <winioctl.h>
#include <mmreg.h>
#include <mmsystem.h>
#include <ks.h>
#include <ksmedia.h>
#include <wil/resource.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace stoneridge {

// Direct User-Mode WDM-KS Kernel Streaming Engine for the Mackie EM-91CU+ (VID_0A73&PID_002E).
// Includes VirtualLock DMA pinning, Dynamic Zero-Crackle IRP Autopilot (2->3->4),
// Hardware Codec PGA/DAC/HID control, and Win11 timer-throttling bypass.
class WdmKsStream final : public IAudioStreamBackend {
public:
    WdmKsStream();
    ~WdmKsStream() override;

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
    uint32_t GetPreferredBufferSize() const noexcept override { return m_bufferSize; }
    uint32_t GetMinBufferSize() const noexcept override { return 32; }
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

    uint32_t GetCaptureUnderruns() const noexcept { return m_captureUnderruns.load(std::memory_order_relaxed); }
    uint32_t GetCaptureTrims() const noexcept { return m_captureTrims.load(std::memory_order_relaxed); }
    uint32_t GetActiveIrpCount() const noexcept { return m_activeIrpCount; }
    DWORD    GetLastPinStatus() const noexcept { return m_lastPinStatus; }

private:
    static constexpr uint32_t kMaxRenderIrps = 4;

    struct IrpSlot {
        KSSTREAM_HEADER   header{};
        OVERLAPPED        ov{};
        wil::unique_event evt;
        std::vector<BYTE> pcm;
        bool              pending = false;
    };

    bool FindMackieKsFilter(std::wstring& outPath, ULONG& outRenderPinId, ULONG& outCapturePinId);
    bool CreateAudioPin(ULONG pinId, bool isCapture, uint32_t sampleRate, uint16_t bits,
                        uint16_t channels, wil::unique_handle& outPin, uint16_t& outActualChannels);
    bool SetPinState(HANDLE hPin, KSSTATE state);
    bool SubmitRenderIrp(IrpSlot& slot);
    bool SubmitCaptureIrp(IrpSlot& slot);
    void CancelAndDrainPins();

    void AudioThreadMain();

    SharedState*   m_sharedState = nullptr;
    ASIOCallbacks* m_callbacks   = nullptr;

    HMODULE m_ksUserDll = nullptr;
    using KsCreatePinFn = DWORD(WINAPI*)(HANDLE, PKSPIN_CONNECT, ACCESS_MASK, PHANDLE);
    KsCreatePinFn m_ksCreatePin = nullptr;

    WdmKsHardwareController m_hwController;
    MackieHidMonitor        m_hidMonitor;
    ScopedHighResTimer      m_timerScope;
    ScopedVirtualLock       m_ramLock;

    std::wstring       m_filterPath;
    ULONG              m_renderPinId  = 0;
    ULONG              m_capturePinId = 1;
    wil::unique_handle m_filterHandle;
    wil::unique_handle m_renderPin;
    wil::unique_handle m_capturePin;
    wil::unique_event  m_stopEvent;

    double   m_sampleRate           = 44100.0;
    uint32_t m_bufferSize           = 128;
    uint32_t m_activeIrpCount       = 3;
    uint16_t m_bitDepth             = 24;
    uint16_t m_inChannels           = 2;
    uint16_t m_outChannels          = 2;
    uint16_t m_capturePinChannels   = 2;
    uint32_t m_renderBytesPerBuffer = 0;
    uint32_t m_captureBytesPerBuffer = 0;
    uint32_t m_supportedRatesMask   = 0b01011;
    uint32_t m_inputLatencySamples  = 128;
    uint32_t m_outputLatencySamples = 256;
    uint32_t m_lastHwCtrlCounter    = 0;

    std::vector<IrpSlot> m_renderIrps;
    std::vector<IrpSlot> m_captureIrps;

    std::vector<float> m_asioIn[2][2];
    std::vector<float> m_asioOut[2][2];
    std::vector<float> m_workIn[2];
    std::vector<float> m_workOut[2];
    LockFreeRingBuffer<float> m_captureRingL;
    LockFreeRingBuffer<float> m_captureRingR;

    InputProcessor         m_inputDsp;
    ControlRoomMonitor     m_controlRoom;
    DacConditioner         m_dacConditioner;
    AdaptiveDriftResampler m_captureAsrc;
    MonitorAuditionContext m_auditionCtx;
    uint32_t               m_adcClipConfirmStreak = 0;
    uint64_t               m_lastAutoPadTickMs    = 0;

    std::thread           m_audioThread;
    std::atomic<bool>     m_running{false};
    std::atomic<uint64_t> m_sampleCounter{0};
    std::atomic<uint64_t> m_lastTimestampNs{0};
    std::atomic<uint32_t> m_captureUnderruns{0};
    std::atomic<uint32_t> m_captureTrims{0};
    DWORD                 m_lastPinStatus = 0;

    uint64_t m_qpcFreq = 1;
};

} // namespace stoneridge
