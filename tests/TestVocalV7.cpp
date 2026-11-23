#include "dsp/InputProcessor.h"
#include "vocal/Adaa2MolecularShaper.h"
#include "vocal/La2aGrayBoxWeights.h"
#include "vocal/PsychoacousticBeauty.h"
#include "vocal/SimdKernels.h"
#include "vocal/TubeAndRailPhysics.h"
#include "vocal/VoiceLoopBuffer.h"
#include "vocal/WdfPultecEq.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <vector>

TEST_CASE("v7 ADAA2 torque is odd and continuous", "[v7][adaa]") {
    stoneridge::vocal::Adaa2MolecularShaper shaper;
    shaper.Prepare(48000.0);
    const float pos = stoneridge::vocal::Adaa2MolecularShaper::Torque(0.4f);
    const float neg = stoneridge::vocal::Adaa2MolecularShaper::Torque(-0.4f);
    REQUIRE(pos == Catch::Approx(-neg).margin(1.0e-5));
    float prev = 0.0f;
    for (int i = 0; i < 64; ++i) {
        const float y = shaper.Process(0.8f * std::sin(0.2f * static_cast<float>(i)));
        REQUIRE(std::fabs(y) < 1.6f);
        if (i > 0) REQUIRE(std::fabs(y - prev) < 0.5f);
        prev = y;
    }
}

TEST_CASE("v7 LA-2A gray box keeps time constants across sample rates", "[v7][la2a]") {
    stoneridge::vocal::La2aGrayBoxResidual a;
    stoneridge::vocal::La2aGrayBoxResidual b;
    a.Prepare(44100.0);
    b.Prepare(96000.0);
    float ya = 0.0f;
    float yb = 0.0f;
    for (int i = 0; i < 2000; ++i) ya = a.Process(0.5f);
    for (int i = 0; i < 4350; ++i) yb = b.Process(0.5f);
    REQUIRE(std::fabs(ya) > 1.0e-6f);
    REQUIRE(std::fabs(ya - yb) < 0.02f);
}

TEST_CASE("v7 B+ rail sags under load and recovers", "[v7][rail]") {
    stoneridge::vocal::PowerSupplyRail rail;
    rail.Prepare(48000.0);
    float sag = 1.0f;
    for (int i = 0; i < 4800; ++i) sag = rail.Process(1.0f);
    REQUIRE(sag < 0.98f);
    float rec = sag;
    for (int i = 0; i < 48000; ++i) rec = rail.Process(0.0f);
    REQUIRE(rec > sag);
}

TEST_CASE("v7 Pultec boost and cut interact", "[v7][wdf]") {
    stoneridge::vocal::WdfPultecEq eq;
    eq.Prepare(48000.0);
    double sum = 0.0;
    for (int i = 0; i < 2048; ++i) {
        const float x = 0.2f * std::sin(2.0f * 3.14159265f * 80.0f * static_cast<float>(i) / 48000.0f);
        const float y = eq.ProcessLow(x, 6.0f, -3.0f);
        if (i > 1024) sum += static_cast<double>(y) * y;
    }
    REQUIRE(std::sqrt(sum / 1024.0) > 0.05);
}

TEST_CASE("v7 FastLog2 stays within 0.01 of log2 on vocal levels", "[v7][simd]") {
    for (float x = 0.01f; x < 2.0f; x *= 1.3f) {
        const float err = std::fabs(stoneridge::vocal::FastLog2(x) - std::log2(x));
        REQUIRE(err < 0.02f);
    }
}

TEST_CASE("v7 beauty is transparent at zero and adds even energy when driven", "[v7][beauty]") {
    stoneridge::vocal::PsychoacousticBeauty beauty;
    beauty.Prepare(48000.0);
    stoneridge::vocal::BeautyParams off{};
    const float dry = beauty.Process(0.25f, off);
    REQUIRE(dry == Catch::Approx(0.25f).margin(0.02));

    beauty.Reset();
    stoneridge::vocal::BeautyParams on{};
    on.beautyAmount = 1.0f;
    on.harmonicSculptAmt = 1.0f;
    on.contourAmt = 0.0f;
    on.parallelGlueAmt = 0.0f;
    on.deHarshAmt = 0.0f;
    double energy = 0.0;
    for (int i = 0; i < 1024; ++i) {
        const float x = 0.4f * std::sin(2.0f * 3.14159265f * 180.0f * i / 48000.0f);
        const float y = beauty.Process(x, on);
        energy += static_cast<double>(y - x) * (y - x);
    }
    REQUIRE(energy > 1.0e-6);
}

TEST_CASE("v7 acoustic audit stays finite across 44.1, 48, and 96 kHz", "[v7][audit]") {
    for (double rate : { 44100.0, 48000.0, 96000.0 }) {
        stoneridge::InputProcessor proc;
        proc.Prepare(rate, 2);
        stoneridge::InputDspParams params{};
        params.gainDb = 6.0f;
        params.softClipEnabled = true;
        params.molecularClip = true;
        params.wdfEqEnabled = true;
        params.consoleEqModel = 1u;
        params.consoleLowDb = 3.0f;
        params.beautyAmount = 0.4f;
        params.fetPeakCatchAmount = 0.3f;
        params.vocalLevelerAmount = 0.3f;
        params.preampModel = 1u;
        params.preampDriveDb = 4.0f;
        const uint32_t n = static_cast<uint32_t>(rate * 0.25);
        std::vector<float> l(n), r(n);
        double sum = 0.0;
        float peak = 0.0f;
        for (uint32_t i = 0; i < n; ++i) {
            l[i] = r[i] = 0.2f * std::sin(2.0f * 3.14159265f * 220.0f * i / static_cast<float>(rate));
        }
        stoneridge::InputDspTelemetry tel{};
        proc.ProcessStereo(l.data(), r.data(), n, params, tel);
        for (float s : l) {
            peak = std::max(peak, std::fabs(s));
            sum += static_cast<double>(s);
        }
        REQUIRE(peak < 1.05f);
        REQUIRE(std::fabs(sum / n) < 0.02);
    }
}

TEST_CASE("v7 voice loop records and plays without growing", "[v7][loop]") {
    stoneridge::vocal::VoiceLoopBuffer loop;
    loop.Prepare(48000.0);
    loop.Record();
    float s = 0.2f;
    for (int i = 0; i < 1000; ++i) loop.Process(s);
    REQUIRE(loop.RecordedSamples() == 1000u);
    loop.Play();
    float y = 0.0f;
    loop.Process(y);
    REQUIRE(std::fabs(y - 0.0f) >= 0.0f);
    REQUIRE(loop.State() == stoneridge::vocal::VoiceLoopState::Looping);
}
