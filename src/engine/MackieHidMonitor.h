#pragma once

#include "ipc/SharedState.h"

#include <windows.h>
#include <wil/resource.h>

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

namespace stoneridge {

// Listens asynchronously to Mackie EM-91CU+ HID endpoints (HID\VID_0A73&PID_002E&MI_03)
// on a low-priority background thread so physical mute button presses sync cleanly
// into SharedState with zero real-time audio-thread impact.
class MackieHidMonitor {
public:
    using MuteCallback       = std::function<void(bool)>;
    using VolumeStepCallback = std::function<void(float)>;

    MackieHidMonitor() = default;
    ~MackieHidMonitor();

    bool Start(SharedState* sharedState,
               MuteCallback onHardwareMute = nullptr,
               VolumeStepCallback onVolumeStep = nullptr);
    void Stop();

    uint32_t GetDeviceCount() const noexcept { return m_openCount; }

private:
    void MonitorLoop();

    SharedState*       m_sharedState = nullptr;
    MuteCallback       m_onMute;
    VolumeStepCallback m_onVolumeStep;
    wil::unique_event  m_stopEvent;
    std::thread        m_thread;
    std::atomic<bool>  m_running{false};
    uint32_t           m_openCount = 0;
};

} // namespace stoneridge
