#include <catch2/catch_test_macros.hpp>
#include "dsp/DacConditioner.h"
#include "dsp/InputProcessor.h"
#include "engine/AudioFormatConvert.h"
#include "engine/RealtimeGuard.h"
#include "ipc/SharedState.h"

#include <algorithm>
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
        std::uniform_real_distribution<float> unit(-1.5f, 3.0f);
        std::uniform_int_distribution<int> coin(0, 15);
        const float nanVal = std::numeric_limits<float>::quiet_NaN();
        const float infVal = std::numeric_limits<float>::infinity();

        auto maybeToxic = [&](float v, int c) noexcept -> float {
            if (c == 0) return nanVal;
            if (c == 1) return infVal;
            return v;
        };

        while (!stopFuzz.load(std::memory_order_relaxed)) {
            const int c = coin(rng);
            shm.inputGainDb.store(maybeToxic(dist(rng), c), std::memory_order_relaxed);
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

            // v7 Analog Rack, Beauty, WDF EQ, ADAA2 & A/B atomics
            shm.micModel.store(static_cast<uint32_t>(c % 7), std::memory_order_relaxed);
            shm.virtualDistanceCm.store(maybeToxic(dist(rng), c ^ 2), std::memory_order_relaxed);
            shm.preampModel.store(static_cast<uint32_t>(c % 6), std::memory_order_relaxed);
            shm.preampDriveDb.store(maybeToxic(dist(rng), c ^ 4), std::memory_order_relaxed);
            shm.transformerIronAmount.store(maybeToxic(unit(rng), c ^ 3), std::memory_order_relaxed);
            shm.consoleEqModel.store(static_cast<uint32_t>(c % 6), std::memory_order_relaxed);
            shm.consoleLowDb.store(maybeToxic(dist(rng), c ^ 5), std::memory_order_relaxed);
            shm.consoleMidDb.store(maybeToxic(dist(rng), c ^ 6), std::memory_order_relaxed);
            shm.consoleAirDb.store(maybeToxic(dist(rng), c ^ 7), std::memory_order_relaxed);
            shm.fetPeakCatchAmount.store(maybeToxic(unit(rng), c ^ 1), std::memory_order_relaxed);
            shm.fetRatioMode.store(static_cast<uint32_t>(c % 4), std::memory_order_relaxed);
            shm.vocalLevelerAmount.store(maybeToxic(unit(rng), c ^ 8), std::memory_order_relaxed);
            shm.beautyAmount.store(maybeToxic(unit(rng), c ^ 9), std::memory_order_relaxed);
            shm.contourAmt.store(maybeToxic(unit(rng), c ^ 10), std::memory_order_relaxed);
            shm.harmonicSculptAmt.store(maybeToxic(unit(rng), c ^ 11), std::memory_order_relaxed);
            shm.deHarshAmt.store(maybeToxic(unit(rng), c ^ 12), std::memory_order_relaxed);
            shm.parallelGlueAmt.store(maybeToxic(unit(rng), c ^ 13), std::memory_order_relaxed);
            shm.wdfEqEnabled.store((c & 1) != 0, std::memory_order_relaxed);
            shm.molecularClip.store((c & 2) != 0, std::memory_order_relaxed);
            shm.ecoMode.store((c & 4) != 0, std::memory_order_relaxed);
            shm.abLevelMatchedBypass.store((c % 5) == 0, std::memory_order_relaxed);
            shm.abAuditionMonitorOnly.store((c & 8) != 0, std::memory_order_relaxed);
            shm.freezeAbxGain.store((c % 7) == 0, std::memory_order_relaxed);
        }
    });

    stoneridge::InputProcessor proc;
    proc.Prepare(48000.0, 2);
    stoneridge::DacConditioner dac;
    dac.Prepare(48000.0);
    stoneridge::InputProcessor::EnableDenormalProtection();

    constexpr uint32_t kBlock = 64;
    std::vector<float> bufL(kBlock), bufR(kBlock);
    std::vector<BYTE> pcm24(kBlock * 6);

    auto sanitizeFloat = [](float v, float lo, float hi, float def) noexcept -> float {
        return std::isfinite(v) ? std::clamp(v, lo, hi) : def;
    };

    bool allOutputsFinite = true;
    {
        // Enter hard real-time trap: ZERO heap allocations permitted inside this scope!
        stoneridge::ScopedRealtimeThread rtGuard;

        for (uint32_t iter = 0; iter < 5000; ++iter) {
            for (uint32_t i = 0; i < kBlock; ++i) {
                bufL[i] = 0.3f * std::sin(0.05f * static_cast<float>(iter * kBlock + i));
                bufR[i] = -0.3f * std::cos(0.05f * static_cast<float>(iter * kBlock + i));
            }

            stoneridge::InputDspParams p{};
            p.gainDb                 = sanitizeFloat(shm.inputGainDb.load(std::memory_order_relaxed), -24.0f, 24.0f, 0.0f);
            p.mute                   = shm.inputMute.load(std::memory_order_relaxed);
            p.softClipEnabled        = shm.softClipEnabled.load(std::memory_order_relaxed);
            p.audioHpfEnabled        = shm.audioHpfEnabled.load(std::memory_order_relaxed);
            p.gateEnabled            = shm.gateEnabled.load(std::memory_order_relaxed);
            p.gateThresholdDb        = sanitizeFloat(shm.gateThresholdDb.load(std::memory_order_relaxed), -80.0f, 0.0f, -42.0f);
            p.gateHysteresisDb       = sanitizeFloat(shm.gateHysteresisDb.load(std::memory_order_relaxed), 0.5f, 18.0f, 4.0f);
            p.gateAttackMs           = sanitizeFloat(shm.gateAttackMs.load(std::memory_order_relaxed), 0.1f, 50.0f, 1.5f);
            p.gateHoldMs             = sanitizeFloat(shm.gateHoldMs.load(std::memory_order_relaxed), 1.0f, 500.0f, 40.0f);
            p.gateReleaseMs          = sanitizeFloat(shm.gateReleaseMs.load(std::memory_order_relaxed), 5.0f, 1000.0f, 120.0f);
            p.gateRangeDb            = sanitizeFloat(shm.gateRangeDb.load(std::memory_order_relaxed), -80.0f, 0.0f, -60.0f);
            p.sidechainHpfHz         = sanitizeFloat(shm.gateSidechainHpfHz.load(std::memory_order_relaxed), 0.0f, 300.0f, 80.0f);

            p.micModel               = std::min<uint32_t>(shm.micModel.load(std::memory_order_relaxed), 5u);
            p.virtualDistanceCm      = sanitizeFloat(shm.virtualDistanceCm.load(std::memory_order_relaxed), 5.0f, 35.0f, 15.0f);
            p.preampModel            = std::min<uint32_t>(shm.preampModel.load(std::memory_order_relaxed), 4u);
            p.preampDriveDb          = sanitizeFloat(shm.preampDriveDb.load(std::memory_order_relaxed), 0.0f, 18.0f, 0.0f);
            p.transformerIronAmount  = sanitizeFloat(shm.transformerIronAmount.load(std::memory_order_relaxed), 0.0f, 1.0f, 0.0f);
            p.consoleEqModel         = std::min<uint32_t>(shm.consoleEqModel.load(std::memory_order_relaxed), 4u);
            p.consoleLowDb           = sanitizeFloat(shm.consoleLowDb.load(std::memory_order_relaxed), -6.0f, 6.0f, 0.0f);
            p.consoleMidDb           = sanitizeFloat(shm.consoleMidDb.load(std::memory_order_relaxed), -6.0f, 6.0f, 0.0f);
            p.consoleAirDb           = sanitizeFloat(shm.consoleAirDb.load(std::memory_order_relaxed), -6.0f, 8.0f, 0.0f);
            p.fetPeakCatchAmount     = sanitizeFloat(shm.fetPeakCatchAmount.load(std::memory_order_relaxed), 0.0f, 1.0f, 0.0f);
            p.fetRatioMode           = std::min<uint32_t>(shm.fetRatioMode.load(std::memory_order_relaxed), 2u);
            p.vocalLevelerAmount     = sanitizeFloat(shm.vocalLevelerAmount.load(std::memory_order_relaxed), 0.0f, 1.0f, 0.0f);
            p.beautyAmount           = sanitizeFloat(shm.beautyAmount.load(std::memory_order_relaxed), 0.0f, 1.0f, 0.0f);
            p.contourAmt             = sanitizeFloat(shm.contourAmt.load(std::memory_order_relaxed), 0.0f, 1.0f, 1.0f);
            p.harmonicSculptAmt      = sanitizeFloat(shm.harmonicSculptAmt.load(std::memory_order_relaxed), 0.0f, 1.0f, 1.0f);
            p.deHarshAmt             = sanitizeFloat(shm.deHarshAmt.load(std::memory_order_relaxed), 0.0f, 1.0f, 1.0f);
            p.parallelGlueAmt        = sanitizeFloat(shm.parallelGlueAmt.load(std::memory_order_relaxed), 0.0f, 1.0f, 1.0f);
            p.wdfEqEnabled           = shm.wdfEqEnabled.load(std::memory_order_relaxed);
            p.molecularClip          = shm.molecularClip.load(std::memory_order_relaxed);
            p.ecoMode                = shm.ecoMode.load(std::memory_order_relaxed);
            p.abLevelMatchedBypass   = shm.abLevelMatchedBypass.load(std::memory_order_relaxed);
            p.abAuditionMonitorOnly  = shm.abAuditionMonitorOnly.load(std::memory_order_relaxed);
            p.freezeAbxGain          = shm.freezeAbxGain.load(std::memory_order_relaxed);

            stoneridge::InputDspTelemetry tel{};
            proc.ProcessStereo(bufL.data(), bufR.data(), kBlock, p, tel);

            stoneridge::DacTelemetry dacTel{};
            dac.ProcessStereoFloat(bufL.data(), bufR.data(), bufL.data(), bufR.data(), kBlock, true, dacTel);
            for (uint32_t i = 0; i < kBlock; ++i) {
                if (!std::isfinite(bufL[i]) || !std::isfinite(bufR[i])) {
                    allOutputsFinite = false;
                }
            }
            stoneridge::ConvertFloatToPcm(bufL.data(), bufR.data(), pcm24.data(), kBlock, 2, 24, false);
        }
    }

    stopFuzz.store(true, std::memory_order_relaxed);
    fuzzer.join();

    REQUIRE(allOutputsFinite);
    // ZERO heap allocations on the real-time audio thread!
    REQUIRE(stoneridge::g_rtAllocViolations.load() == 0);
}
