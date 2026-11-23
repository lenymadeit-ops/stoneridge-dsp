#include <catch2/catch_test_macros.hpp>
#include "dsp/InputProcessor.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>

TEST_CASE("Benchmark InputProcessor 64-sample @ 96kHz overhead", "[benchmark]") {
    stoneridge::InputProcessor::EnableDenormalProtection();
    stoneridge::InputProcessor proc;
    proc.Prepare(96000.0, 2);

    stoneridge::InputDspParams params{};
    params.gainDb          = 6.0f;
    params.softClipEnabled = true;
    params.gateEnabled     = true;
    params.gateThresholdDb = -40.0f;
    params.sidechainHpfHz  = 90.0f;

    constexpr uint32_t kBlockSize = 64;
    constexpr uint32_t kIterations = 10000; // ~6.67 seconds of 96kHz audio
    std::vector<float> left(kBlockSize, 0.1f);
    std::vector<float> right(kBlockSize, -0.1f);
    stoneridge::InputDspTelemetry tel{};

    const auto start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kIterations; ++i) {
        proc.ProcessStereo(left.data(), right.data(), kBlockSize, params, tel);
    }
    const auto end = std::chrono::steady_clock::now();

    const double elapsedUs = std::chrono::duration<double, std::micro>(end - start).count();
    const double usPerBlock = elapsedUs / kIterations;
    const double budgetUs = (static_cast<double>(kBlockSize) / 96000.0) * 1e6; // 666.67 us
    const double cpuPct = (usPerBlock / budgetUs) * 100.0;

    std::cout << "[Benchmark] 64-sample @ 96kHz DSP: " << usPerBlock
              << " us/block (" << cpuPct << "% of real-time budget)\n";

    // Must stay well under 5% of a single core's real-time budget
    REQUIRE(cpuPct < 5.0);
}
