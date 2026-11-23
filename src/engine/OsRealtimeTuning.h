#pragma once

#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>

#include <cstdint>
#include <vector>

namespace stoneridge {

// Disables Windows 11 background timer throttling (PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION)
// so timeBeginPeriod / NtSetTimerResolution stay active when FL Studio loses focus or is minimized.
inline void DisableWin11TimerThrottling() noexcept {
    constexpr ULONG kControlIgnoreTimerRes = 0x4; // PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = kControlIgnoreTimerRes;
    state.StateMask   = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
}

// RAII scope raising Windows kernel timer resolution to 0.5 ms via ntdll!NtSetTimerResolution
// (plus timeBeginPeriod(1) fallback) while an audio stream is running.
class ScopedHighResTimer {
public:
    ScopedHighResTimer() noexcept = default;
    ~ScopedHighResTimer() noexcept { Release(); }

    ScopedHighResTimer(const ScopedHighResTimer&) = delete;
    ScopedHighResTimer& operator=(const ScopedHighResTimer&) = delete;

    void Acquire() noexcept {
        if (m_active) return;
        DisableWin11TimerThrottling();
        timeBeginPeriod(1);

        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
            using NtSetTimerResolutionFn = LONG(NTAPI*)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);
            auto fn = reinterpret_cast<NtSetTimerResolutionFn>(GetProcAddress(ntdll, "NtSetTimerResolution"));
            if (fn) {
                ULONG current = 0;
                if (fn(5000, TRUE, &current) >= 0) { // 5000 * 100ns = 0.5 ms
                    m_ntTimerActive = true;
                }
            }
        }
        m_active = true;
    }

    void Release() noexcept {
        if (!m_active) return;
        if (m_ntTimerActive) {
            if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
                using NtSetTimerResolutionFn = LONG(NTAPI*)(ULONG, BOOLEAN, PULONG);
                auto fn = reinterpret_cast<NtSetTimerResolutionFn>(GetProcAddress(ntdll, "NtSetTimerResolution"));
                if (fn) {
                    ULONG current = 0;
                    fn(5000, FALSE, &current);
                }
            }
            m_ntTimerActive = false;
        }
        timeEndPeriod(1);
        m_active = false;
    }

private:
    bool m_active        = false;
    bool m_ntTimerActive = false;
};

// Queries CPU core topology via GetLogicalProcessorInformationEx:
// - On Intel/AMD hybrid CPUs: picks a Performance core (highest EfficiencyClass > 0).
// - On symmetric CPUs (e.g. Intel i5-8365U 4C/8T): picks the highest physical non-Core-0 core
//   via SetThreadIdealProcessorEx so the audio thread avoids Core 0 USB/ACPI/GPU DPC storms
//   without restricting Windows from migrating if another thread monopolizes that core.
inline void ApplyOptimalAudioThreadPriority(bool enablePCore) noexcept {
    if (!enablePCore) return;

    THREAD_POWER_THROTTLING_STATE throttle{};
    throttle.Version     = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    throttle.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    throttle.StateMask   = 0;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttle, sizeof(throttle));

    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len == 0) return;

    std::vector<BYTE> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()),
                                          &len)) {
        return;
    }

    struct CoreCandidate {
        WORD      group;
        BYTE      lpNumber;
        BYTE      effClass;
    };
    std::vector<CoreCandidate> cores;
    BYTE maxEffClass = 0;

    BYTE* ptr = buf.data();
    BYTE* end = buf.data() + len;
    while (ptr < end) {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(ptr);
        if (info->Relationship == RelationProcessorCore && info->Processor.GroupCount > 0) {
            const auto& grp = info->Processor.GroupMask[0];
            DWORD bitIndex = 0;
            if (BitScanForward64(&bitIndex, grp.Mask)) {
                const BYTE eff = info->Processor.EfficiencyClass;
                maxEffClass = (eff > maxEffClass) ? eff : maxEffClass;
                cores.push_back({ grp.Group, static_cast<BYTE>(bitIndex), eff });
            }
        }
        ptr += info->Size;
    }

    if (cores.empty()) return;

    // Prefer highest EfficiencyClass and avoid Logical Processor 0 (Core 0 DPCs)
    CoreCandidate best = cores.back();
    for (auto it = cores.rbegin(); it != cores.rend(); ++it) {
        if (it->effClass == maxEffClass && it->lpNumber != 0) {
            best = *it;
            break;
        }
    }

    PROCESSOR_NUMBER procNum{};
    procNum.Group  = best.group;
    procNum.Number = best.lpNumber;
    SetThreadIdealProcessorEx(GetCurrentThread(), &procNum, nullptr);
}

// RAII physical RAM pinning for real-time audio & DMA buffers (prevents paging faults under heavy VST load)
class ScopedVirtualLock {
public:
    ScopedVirtualLock() noexcept = default;
    ~ScopedVirtualLock() noexcept { UnlockAll(); }

    ScopedVirtualLock(const ScopedVirtualLock&) = delete;
    ScopedVirtualLock& operator=(const ScopedVirtualLock&) = delete;

    void Lock(void* addr, size_t bytes) noexcept {
        if (!addr || bytes == 0) return;
        if (VirtualLock(addr, bytes)) {
            m_regions.push_back({ addr, bytes });
        }
    }

    void UnlockAll() noexcept {
        for (const auto& r : m_regions) {
            VirtualUnlock(r.addr, r.bytes);
        }
        m_regions.clear();
    }

private:
    struct Region { void* addr; size_t bytes; };
    std::vector<Region> m_regions;
};

} // namespace stoneridge
