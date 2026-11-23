#pragma once

#include <atomic>
#include <cstdint>

namespace stoneridge {

// Thread-local real-time audio guard.
// In QA builds (STONERIDGE_RT_CHECK), global operator new/delete checks this flag
// and increments a violation counter (or aborts) if ANY heap allocation occurs
// on a real-time audio thread.
inline thread_local bool g_isRealtimeAudioThread = false;
inline std::atomic<uint64_t> g_rtAllocViolations{0};

struct ScopedRealtimeThread {
    bool prev;
    ScopedRealtimeThread() noexcept : prev(g_isRealtimeAudioThread) {
        g_isRealtimeAudioThread = true;
    }
    ~ScopedRealtimeThread() noexcept {
        g_isRealtimeAudioThread = prev;
    }
};

} // namespace stoneridge
