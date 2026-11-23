#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "dsp/InputProcessor.h"
#include "engine/AudioFormatConvert.h"

#include <cmath>
#include <vector>

TEST_CASE("Continuous 2000-block phase coherence and zipper-free gain sweep", "[continuity]") {
    stoneridge::InputProcessor::EnableDenormalProtection();
    stoneridge::InputProcessor proc;
    constexpr double kRate = 48000.0;
    proc.Prepare(kRate, 2);

    constexpr uint32_t kBlock = 64;
    constexpr uint32_t kNumBlocks = 2000;
    constexpr double kFreq = 440.0;
    constexpr double kTwoPi = 6.28318530717958647692;

    std::vector<float> allL;
    allL.reserve(kBlock * kNumBlocks);
    std::vector<float> bufL(kBlock), bufR(kBlock);
    stoneridge::InputDspParams params{};
    params.softClipEnabled = true;
    params.audioHpfEnabled = false;
    params.gateEnabled     = false;

    uint64_t sampleIdx = 0;
    for (uint32_t b = 0; b < kNumBlocks; ++b) {
        // Sweep input gain smoothly from -6 dB to +6 dB across blocks
        params.gainDb = -6.0f + 12.0f * (static_cast<float>(b) / kNumBlocks);
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float s = 0.25f * static_cast<float>(std::sin(kTwoPi * kFreq * static_cast<double>(sampleIdx++) / kRate));
            bufL[i] = s;
            bufR[i] = s;
        }
        stoneridge::InputDspTelemetry tel{};
        proc.ProcessStereo(bufL.data(), bufR.data(), kBlock, params, tel);
        allL.insert(allL.end(), bufL.begin(), bufL.end());
    }

    // 2nd-derivative click/zipper detector across all 128,000 samples (including every 64-sample boundary!)
    float maxSecondDiff = 0.0f;
    for (size_t i = 256; i + 1 < allL.size(); ++i) {
        const float d2 = std::fabs(allL[i + 1] - 2.0f * allL[i] + allL[i - 1]);
        maxSecondDiff = std::max(maxSecondDiff, d2);
    }
    // For a clean 440Hz sine @ 48kHz, theoretical |d2| <= A * (2*pi*440/48000)^2 ~= 0.0017
    REQUIRE(maxSecondDiff < 0.01f);
}

TEST_CASE("AVX2 Direct Monitor Blend and Peak Detector parity across odd frame counts", "[continuity]") {
    for (uint32_t frames : { 1u, 7u, 63u, 64u, 133u, 144u, 257u }) {
        std::vector<float> flL(frames), flR(frames), micL(frames), micR(frames), dstL(frames), dstR(frames);
        float refPeak = 0.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            flL[i]  = 0.4f * std::sin(0.1f * i);
            flR[i]  = -0.35f * std::cos(0.13f * i);
            micL[i] = 0.3f * std::sin(0.21f * i);
            micR[i] = 0.25f * std::cos(0.17f * i);
        }

        stoneridge::BlendDirectMonitorAvx2(flL.data(), micL.data(), dstL.data(), frames, 0.5f);
        stoneridge::BlendDirectMonitorAvx2(flR.data(), micR.data(), dstR.data(), frames, 0.5f);

        for (uint32_t i = 0; i < frames; ++i) {
            const float expL = std::clamp(flL[i] + micL[i] * 0.5f, -1.0f, 1.0f);
            const float expR = std::clamp(flR[i] + micR[i] * 0.5f, -1.0f, 1.0f);
            REQUIRE(dstL[i] == Catch::Approx(expL).margin(1e-5f));
            REQUIRE(dstR[i] == Catch::Approx(expR).margin(1e-5f));
            refPeak = std::max(refPeak, std::max(std::fabs(dstL[i]), std::fabs(dstR[i])));
        }

        const float avxPeak = stoneridge::ComputeStereoPeakAvx2(dstL.data(), dstR.data(), frames);
        REQUIRE(avxPeak == Catch::Approx(refPeak).margin(1e-5f));
    }
}
