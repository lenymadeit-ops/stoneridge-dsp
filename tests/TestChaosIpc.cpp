#include <catch2/catch_test_macros.hpp>
#include "dsp/InputProcessor.h"
#include "engine/AudioFormatConvert.h"
#include "engine/RealtimeGuard.h"
#include "ipc/SharedState.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <random>
#include <thread>
#include <vector>

// Global operator new override in test binary to trap ANY real-time audio-thread heap allocation!
void* operator new(std::size_t size) {
    if (stoneridge::g_isRealtimeAudioThread) {
        stoneridge::g_rtAllocViolations.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

TEST_CASE("Chaos SharedState IPC Fuzzing + Zero-Allocation Real-Time Audio Trap", "[chaos]") {
    stoneridge::SharedState shm{};
    stoneridge::InitializeDefaultSharedState(shm);
    stoneridge::g_rtAllocViolations.store(0, std::memory_order_relaxed);

    std::atomic<bool> stopFuzz{false};
    std::thread fuzzer([&]() {
        std::mt19937 rng(42);
        std::uniform_real_distribution<float> dist(-200.0f, 200.0f);
        std::uniform_int_distribution<int> coin(0, 9);
        const float nanVal = std::numeric_limits<float>::quiet_NaN();
        const float infVal = std::numeric_limits<float>::infinity();

        while (!stopFuzz.load(std::memory_order_relaxed)) {
            const int c = coin(rng);
            shm.inputGainDb.store((c == 0) ? nanVal : (c == 1 ? infVal : dist(rng)), std::memory_order_relaxed);
            shm.inputMute.store((c & 1) != 0, std::memory_order_relaxed);
            shm.softClipEnabled.store((c & 2) != 0, std::memory_order_relaxed);
            shm.audioHpfEnabled.store((c & 4) != 0, std::memory_order_relaxed);
            shm.gateEnabled.store((c % 3) == 0, std::memory_order_relaxed);
            shm.gateThresholdDb.store(dist(rng), std::memory_order_relaxed);
            shm.gateHysteresisDb.store(dist(rng), std::memory_order_relaxed);
            shm.gateAttackMs.store(dist(rng), std::memory_order_relaxed);
            shm.gateHoldMs.store(dist(rng), std::memory_order_relaxed);
            shm.gateReleaseMs.store(dist(rng), std::memory_order_relaxed);
            shm.gateRangeDb.store(dist(rng), std::memory_order_relaxed);
            shm.gateSidechainHpfHz.store(dist(rng), std::memory_order_relaxed);
        }
    });

    stoneridge::InputProcessor proc;
    proc.Prepare(48000.0, 2);
    stoneridge::InputProcessor::EnableDenormalProtection();

    constexpr uint32_t kBlock = 64;
    std::vector<float> bufL(kBlock), bufR(kBlock);
    std::vector<BYTE> pcm24(kBlock * 6);

    {
        // Enter hard real-time trap: ZERO heap allocations permitted inside this scope!
        stoneridge::ScopedRealtimeThread rtGuard;

        for (uint32_t iter = 0; iter < 5000; ++iter) {
            for (uint32_t i = 0; i < kBlock; ++i) {
                bufL[i] = 0.3f * std::sin(0.05f * static_cast<float>(iter * kBlock + i));
                bufR[i] = -0.3f * std::cos(0.05f * static_cast<float>(iter * kBlock + i));
            }

            stoneridge::InputDspParams p{};
            const float rawGain = shm.inputGainDb.load(std::memory_order_relaxed);
            p.gainDb           = std::isfinite(rawGain) ? rawGain : 0.0f;
            p.mute             = shm.inputMute.load(std::memory_order_relaxed);
            p.softClipEnabled  = shm.softClipEnabled.load(std::memory_order_relaxed);
            p.audioHpfEnabled  = shm.audioHpfEnabled.load(std::memory_order_relaxed);
            p.gateEnabled      = shm.gateEnabled.load(std::memory_order_relaxed);
            p.gateThresholdDb  = shm.gateThresholdDb.load(std::memory_order_relaxed);
            p.gateHysteresisDb = shm.gateHysteresisDb.load(std::memory_order_relaxed);
            p.gateAttackMs     = shm.gateAttackMs.load(std::memory_order_relaxed);
            p.gateHoldMs       = shm.gateHoldMs.load(std::memory_order_relaxed);
            p.gateReleaseMs    = shm.gateReleaseMs.load(std::memory_order_relaxed);
            p.gateRangeDb      = shm.gateRangeDb.load(std::memory_order_relaxed);
            p.sidechainHpfHz   = shm.gateSidechainHpfHz.load(std::memory_order_relaxed);

            stoneridge::InputDspTelemetry tel{};
            proc.ProcessStereo(bufL.data(), bufR.data(), kBlock, p, tel);
            stoneridge::ConvertFloatToPcm(bufL.data(), bufR.data(), pcm24.data(), kBlock, 2, 24, false);
        }
    }

    stopFuzz.store(true, std::memory_order_relaxed);
    fuzzer.join();

    // ZERO heap allocations on the real-time audio thread!
    REQUIRE(stoneridge::g_rtAllocViolations.load() == 0);
}
