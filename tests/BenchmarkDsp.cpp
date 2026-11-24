#include <catch2/catch_test_macros.hpp>
#include "dsp/DacConditioner.h"
#include "dsp/InputProcessor.h"
#include "plugin/VocalChainPresets.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>

namespace {

template <typename BlockFn>
double MeasureMedianUsPerBlock(BlockFn&& blockFn) {
    constexpr uint32_t kBatches = 20;
    constexpr uint32_t kBatchIters = 500;
    std::array<double, kBatches> batchUsPerBlock{};
    uint32_t blockIdx = 0;
    for (uint32_t batch = 0; batch < kBatches; ++batch) {
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t b = 0; b < kBatchIters; ++b, ++blockIdx) {
            blockFn(blockIdx);
        }
        const auto end = std::chrono::steady_clock::now();
        const double elapsedUs = std::chrono::duration<double, std::micro>(end - start).count();
        batchUsPerBlock[batch] = elapsedUs / static_cast<double>(kBatchIters);
    }
    std::sort(batchUsPerBlock.begin(), batchUsPerBlock.end());
    return batchUsPerBlock[kBatches / 2];
}

}  // namespace

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
    std::vector<float> left(kBlockSize, 0.1f);
    std::vector<float> right(kBlockSize, -0.1f);
    stoneridge::InputDspTelemetry tel{};

    const double usPerBlock = MeasureMedianUsPerBlock([&](uint32_t) {
        proc.ProcessStereo(left.data(), right.data(), kBlockSize, params, tel);
    });
    const double budgetUs = (static_cast<double>(kBlockSize) / 96000.0) * 1e6;
    const double cpuPct = (usPerBlock / budgetUs) * 100.0;

    std::cout << "[Benchmark] 64-sample @ 96kHz DSP: " << usPerBlock
              << " us/block (" << cpuPct << "% of real-time budget)\n";

    REQUIRE(cpuPct < 3.5);
}

TEST_CASE("Benchmark Live Mono Vocal Tracking (Modern Pop Gloss)", "[benchmark][v7]") {
    stoneridge::InputProcessor::EnableDenormalProtection();
    constexpr uint32_t kBlockSize = 64;
    constexpr uint32_t kWarmup = 500;
    constexpr uint32_t kTableLen = 1024u;
    std::vector<float> sineTable(kTableLen, 0.0f);
    for (uint32_t j = 0; j < kTableLen; ++j) {
        sineTable[j] = 0.15f * std::sin(0.03f * static_cast<float>(j));
    }

    auto measureMono = [&](double rate, bool eco, const char* label) -> double {
        stoneridge::InputProcessor proc;
        stoneridge::DacConditioner dac;
        proc.Prepare(rate, 1);
        dac.Prepare(rate);

        stoneridge::InputDspParams params{};
        stoneridge::ApplyVocalPreset(params, stoneridge::PresetByIndex(0));
        params.ecoMode = eco;

        std::vector<float> mono(kBlockSize, 0.1f);
        stoneridge::InputDspTelemetry tel{};
        stoneridge::DacTelemetry dacTel{};

        for (uint32_t i = 0; i < kWarmup; ++i) {
            proc.ProcessMono(mono.data(), kBlockSize, params, tel);
            dac.ProcessStereoFloat(mono.data(), nullptr, mono.data(), nullptr, kBlockSize, true, dacTel);
        }

        const double usPerBlock = MeasureMedianUsPerBlock([&](uint32_t blockIdx) {
            for (uint32_t j = 0; j < kBlockSize; ++j) {
                mono[j] = sineTable[(blockIdx * kBlockSize + j) & (kTableLen - 1u)];
            }
            proc.ProcessMono(mono.data(), kBlockSize, params, tel);
            dac.ProcessStereoFloat(mono.data(), nullptr, mono.data(), nullptr, kBlockSize, true, dacTel);
        });

        const double budgetUs = (static_cast<double>(kBlockSize) / rate) * 1e6;
        const double cpuPct = (usPerBlock / budgetUs) * 100.0;
        std::cout << "[Benchmark] Live Mono " << label << ": " << usPerBlock
                  << " us/block (" << cpuPct << "% of real-time budget)\n";
        return cpuPct;
    };

    const double mono48EcoPct = measureMono(48000.0, true,  "48kHz ECO (1x->48k)");
    const double mono48HqPct  = measureMono(48000.0, false, "48kHz HQ (4x->192k)");
    const double mono96EcoPct = measureMono(96000.0, true,  "96kHz ECO (1x->96k)");
    const double mono96HqPct  = measureMono(96000.0, false, "96kHz HQ (2x->192k)");
    REQUIRE(mono48EcoPct < 6.0);
    REQUIRE(mono48HqPct < 10.0);
    REQUIRE(mono96EcoPct < 10.0);
    REQUIRE(mono96HqPct < 12.0);
}

TEST_CASE("Benchmark Full v7 Analog Chain Stereo (Modern Pop Gloss) @ 96kHz and 48kHz", "[benchmark][v7]") {
    stoneridge::InputProcessor::EnableDenormalProtection();
    constexpr uint32_t kTableLen = 1024u;
    std::vector<float> sineTable(kTableLen, 0.0f);
    for (uint32_t j = 0; j < kTableLen; ++j) {
        sineTable[j] = 0.15f * std::sin(0.03f * static_cast<float>(j));
    }

    auto measureStereo = [&](double rate, uint32_t blockSize) -> double {
        stoneridge::InputProcessor proc;
        stoneridge::DacConditioner dac;
        proc.Prepare(rate, 2);
        dac.Prepare(rate);

        stoneridge::InputDspParams params{};
        stoneridge::ApplyVocalPreset(params, stoneridge::PresetByIndex(0));

        constexpr uint32_t kWarmup = 500;
        std::vector<float> left(blockSize, 0.1f);
        std::vector<float> right(blockSize, -0.1f);
        stoneridge::InputDspTelemetry tel{};
        stoneridge::DacTelemetry dacTel{};

        for (uint32_t i = 0; i < kWarmup; ++i) {
            proc.ProcessStereo(left.data(), right.data(), blockSize, params, tel);
            dac.ProcessStereoFloat(left.data(), right.data(), left.data(), right.data(), blockSize, true, dacTel);
        }

        const double usPerBlock = MeasureMedianUsPerBlock([&](uint32_t blockIdx) {
            for (uint32_t j = 0; j < blockSize; ++j) {
                const float x = sineTable[(blockIdx * blockSize + j) & (kTableLen - 1u)];
                left[j] = x;
                right[j] = x;
            }
            proc.ProcessStereo(left.data(), right.data(), blockSize, params, tel);
            dac.ProcessStereoFloat(left.data(), right.data(), left.data(), right.data(), blockSize, true, dacTel);
        });

        const double budgetUs = (static_cast<double>(blockSize) / rate) * 1e6;
        const double cpuPct = (usPerBlock / budgetUs) * 100.0;
        std::cout << "[Benchmark] Stereo v7 Modern Pop Gloss " << blockSize << "-sample @ "
                  << static_cast<int>(rate) << "Hz: " << usPerBlock
                  << " us/block (" << cpuPct << "% of real-time budget)\n";
        return cpuPct;
    };

    const double stereo48Pct = measureStereo(48000.0, 64u);
    const double stereo96Pct = measureStereo(96000.0, 64u);
    REQUIRE(stereo48Pct < 16.0);
    REQUIRE(stereo96Pct < 19.0);
}
