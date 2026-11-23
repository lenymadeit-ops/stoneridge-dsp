#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "dsp/InputProcessor.h"

#include <cmath>
#include <vector>

TEST_CASE("InputProcessor gain and soft-clipper", "[dsp]") {
    stoneridge::InputProcessor proc;
    proc.Prepare(48000.0, 2);

    stoneridge::InputDspParams params{};
    params.gainDb          = 6.0206f; // ~2x
    params.softClipEnabled = false;
    params.gateEnabled     = false;

    stoneridge::InputDspTelemetry tel{};
    std::vector<float> left(2048), right(2048);
    for (size_t i = 0; i < 2048; ++i) {
        const float s = 0.25f * std::sin(2.0f * 3.14159265f * 1000.0f * static_cast<float>(i) / 48000.0f);
        left[i] = right[i] = s;
    }

    proc.ProcessStereo(left.data(), right.data(), 2048, params, tel);

    // After 5ms smoothing settles, 0.25 amplitude boosted by +6.02 dB (2x) -> 0.50 peak
    float peak = 0.0f;
    for (size_t i = 1024; i < 2048; ++i) {
        peak = std::max(peak, std::fabs(left[i]));
    }
    REQUIRE(peak == Catch::Approx(0.5f).margin(0.01f));

    // Soft-clipper ceiling check
    params.gainDb          = 24.0f;
    params.softClipEnabled = true;
    std::fill(left.begin(), left.end(), 0.8f);
    std::fill(right.begin(), right.end(), 0.8f);
    proc.ProcessStereo(left.data(), right.data(), 2048, params, tel);
    REQUIRE(left.back() <= 0.97f);
}

TEST_CASE("InputProcessor noise gate attenuation and denormal safety", "[dsp]") {
    stoneridge::InputProcessor::EnableDenormalProtection();
    stoneridge::InputProcessor proc;
    proc.Prepare(48000.0, 2);

    stoneridge::InputDspParams params{};
    params.gainDb          = 0.0f;
    params.softClipEnabled = false;
    params.gateEnabled     = true;
    params.gateThresholdDb = -30.0f;
    params.gateHysteresisDb = 4.0f;
    params.gateAttackMs    = 1.0f;
    params.gateHoldMs      = 10.0f;
    params.gateReleaseMs   = 20.0f;
    params.gateRangeDb     = -60.0f;
    params.sidechainHpfHz  = 80.0f;

    stoneridge::InputDspTelemetry tel{};

    // Feed sub-threshold quiet signal (and tiny denormal-range values) for 100ms
    std::vector<float> left(4800, 1e-38f);
    std::vector<float> right(4800, 1e-38f);
    proc.ProcessStereo(left.data(), right.data(), 4800, params, tel);

    REQUIRE(tel.gateIsOpen == false);
    REQUIRE(tel.gateReductionDb < -40.0f);
    for (float s : left) {
        REQUIRE(std::isfinite(s));
    }

    // Feed loud 1 kHz tone above threshold -> gate should open
    for (size_t i = 0; i < 4800; ++i) {
        const float v = 0.5f * std::sin(2.0f * 3.14159265f * 1000.0f * static_cast<float>(i) / 48000.0f);
        left[i]  = v;
        right[i] = v;
    }
    proc.ProcessStereo(left.data(), right.data(), 4800, params, tel);
    REQUIRE(tel.gateIsOpen == true);

    // Process a subsequent block once the 1ms attack has settled -> attenuation should be ~0 dB
    std::vector<float> settledL(128, 0.5f);
    std::vector<float> settledR(128, 0.5f);
    proc.ProcessStereo(settledL.data(), settledR.data(), 128, params, tel);
    REQUIRE(tel.gateReductionDb > -1.0f);
}

TEST_CASE("InputProcessor 80 Hz audio rumble HPF attenuates 20 Hz and passes 1 kHz", "[dsp]") {
    stoneridge::InputProcessor proc;
    proc.Prepare(48000.0, 2);

    stoneridge::InputDspParams params{};
    params.gainDb          = 0.0f;
    params.softClipEnabled = false;
    params.audioHpfEnabled = true;
    params.gateEnabled     = false;

    stoneridge::InputDspTelemetry tel{};
    constexpr uint32_t kN = 4800;
    std::vector<float> l(kN), r(kN);

    // 20 Hz sub-rumble (2 octaves below 80 Hz -> ~24 dB attenuation)
    for (uint32_t i = 0; i < kN; ++i) {
        const float v = 0.5f * std::sin(2.0f * 3.14159265f * 20.0f * static_cast<float>(i) / 48000.0f);
        l[i] = r[i] = v;
    }
    proc.ProcessStereo(l.data(), r.data(), kN, params, tel);
    float peak20 = 0.0f;
    for (uint32_t i = kN / 2; i < kN; ++i) peak20 = std::max(peak20, std::fabs(l[i]));
    REQUIRE(peak20 < 0.06f); // > 18 dB attenuated from 0.5

    // 1 kHz vocal tone -> should pass nearly untouched (~0.5)
    for (uint32_t i = 0; i < kN; ++i) {
        const float v = 0.5f * std::sin(2.0f * 3.14159265f * 1000.0f * static_cast<float>(i) / 48000.0f);
        l[i] = r[i] = v;
    }
    proc.ProcessStereo(l.data(), r.data(), kN, params, tel);
    float peak1k = 0.0f;
    for (uint32_t i = kN / 2; i < kN; ++i) peak1k = std::max(peak1k, std::fabs(l[i]));
    REQUIRE(peak1k == Catch::Approx(0.5f).margin(0.02f));
}
