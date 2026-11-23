#include <catch2/catch_test_macros.hpp>
#include "engine/AudioFormatConvert.h"

#include <cmath>
#include <limits>
#include <vector>

namespace {

constexpr double kTwoPi = 6.28318530717958647692;

double MeasureSqnrDb(uint16_t bits, bool isFloat) {
    constexpr uint32_t kFrames = 4096;
    std::vector<float> inL(kFrames), inR(kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) {
        inL[i] = static_cast<float>(0.9 * std::sin(kTwoPi * 997.0 * i / 48000.0));
        inR[i] = static_cast<float>(0.9 * std::cos(kTwoPi * 1237.0 * i / 48000.0));
    }

    std::vector<BYTE> pcm(kFrames * 2 * (bits / 8), 0);
    std::vector<float> outL(kFrames, 0.0f), outR(kFrames, 0.0f);

    stoneridge::ConvertFloatToPcm(inL.data(), inR.data(), pcm.data(), kFrames, 2, bits, isFloat);
    stoneridge::ConvertPcmToFloat(pcm.data(), outL.data(), outR.data(), kFrames, 2, bits, isFloat);

    double sigPower = 0.0, errPower = 0.0;
    for (uint32_t i = 0; i < kFrames; ++i) {
        const double eL = static_cast<double>(outL[i]) - static_cast<double>(inL[i]);
        const double eR = static_cast<double>(outR[i]) - static_cast<double>(inR[i]);
        sigPower += inL[i] * inL[i] + inR[i] * inR[i];
        errPower += eL * eL + eR * eR;
    }
    return 10.0 * std::log10(sigPower / std::max(errPower, 1e-20));
}

} // namespace

TEST_CASE("PCM format conversion bit-exact SQNR across 16/24/32-bit", "[format]") {
    REQUIRE(MeasureSqnrDb(16, false) > 92.0);
    REQUIRE(MeasureSqnrDb(24, false) > 138.0);
    REQUIRE(MeasureSqnrDb(32, false) > 140.0);
    REQUIRE(MeasureSqnrDb(32, true)  > 150.0);
}

TEST_CASE("PCM conversion NaN, Inf, and hard-clipping safety (zero wraparound pops)", "[format]") {
    const float nanVal = std::numeric_limits<float>::quiet_NaN();
    const float infVal = std::numeric_limits<float>::infinity();

    std::vector<float> inL = { -5.0f, -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 5.0f, nanVal, infVal, -infVal };
    std::vector<float> inR = {  5.0f,  1.0f,  0.5f, 0.0f,-0.5f,-1.0f,-5.0f, -infVal, nanVal, infVal };
    const uint32_t n = static_cast<uint32_t>(inL.size());

    for (uint16_t bits : { uint16_t{16}, uint16_t{24}, uint16_t{32} }) {
        std::vector<BYTE> pcm(n * 2 * (bits / 8), 0);
        std::vector<float> outL(n, 0.0f), outR(n, 0.0f);

        stoneridge::ConvertFloatToPcm(inL.data(), inR.data(), pcm.data(), n, 2, bits, false);
        stoneridge::ConvertPcmToFloat(pcm.data(), outL.data(), outR.data(), n, 2, bits, false);

        // Verify -5.0f clamped cleanly near -1.0f (NOT wrapped to +1.0f!) and +5.0f near +1.0f
        REQUIRE(outL[0] < -0.99f);
        REQUIRE(outL[6] >  0.99f);
        REQUIRE(outR[0] >  0.99f);
        REQUIRE(outR[6] < -0.99f);

        // Verify zero NaNs/Infs survive
        for (uint32_t i = 0; i < n; ++i) {
            REQUIRE(std::isfinite(outL[i]));
            REQUIRE(std::isfinite(outR[i]));
        }
    }
}

TEST_CASE("AVX2 24-bit pshufb parity across odd frame counts and mono", "[format]") {
    for (uint32_t frames : { 1u, 3u, 7u, 15u, 63u, 133u, 257u }) {
        std::vector<float> inL(frames), inR(frames), outL(frames), outR(frames);
        for (uint32_t i = 0; i < frames; ++i) {
            inL[i] = 0.75f * std::sin(0.17f * static_cast<float>(i));
            inR[i] = -0.65f * std::cos(0.23f * static_cast<float>(i));
        }

        std::vector<BYTE> pcm24(frames * 6, 0);
        stoneridge::ConvertFloatToPcm(inL.data(), inR.data(), pcm24.data(), frames, 2, 24, false);
        stoneridge::ConvertPcmToFloat(pcm24.data(), outL.data(), outR.data(), frames, 2, 24, false);

        for (uint32_t i = 0; i < frames; ++i) {
            REQUIRE(std::fabs(outL[i] - inL[i]) < 1e-5f);
            REQUIRE(std::fabs(outR[i] - inR[i]) < 1e-5f);
        }

        // Also verify 1ch mono 24-bit round-trip
        std::vector<BYTE> mono24(frames * 3, 0);
        stoneridge::ConvertFloatToPcm(inL.data(), inL.data(), mono24.data(), frames, 1, 24, false);
        stoneridge::ConvertPcmToFloat(mono24.data(), outL.data(), outR.data(), frames, 1, 24, false);
        for (uint32_t i = 0; i < frames; ++i) {
            REQUIRE(std::fabs(outL[i] - inL[i]) < 1e-5f);
            REQUIRE(std::fabs(outR[i] - inL[i]) < 1e-5f);
        }
    }
}
