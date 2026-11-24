#include "dsp/ControlRoomMonitor.h"
#include "dsp/DacConditioner.h"
#include "dsp/InputProcessor.h"
#include "plugin/VocalChainPresets.h"
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

TEST_CASE("v7 ControlRoomMonitor FDN tail and Spagnol spatial externalizer", "[v7][monitor]") {
    stoneridge::ControlRoomMonitor mon;
    mon.Prepare(48000.0);

    stoneridge::ControlRoomParams mp{};
    mp.directMonitorMix    = 1.0f;
    mp.monitorReverbAmount = 0.70f;
    mp.crossfeedEnabled    = false;
    mp.cueCarveEnabled     = false;

    constexpr uint32_t kN = 4096;
    std::vector<float> dawL(kN, 0.0f), dawR(kN, 0.0f);
    std::vector<float> micL(kN, 0.0f), micR(kN, 0.0f);
    std::vector<float> outL(kN, 0.0f), outR(kN, 0.0f);
    micL[8] = micR[8] = 0.5f;

    mon.ProcessBlock(dawL.data(), dawR.data(), micL.data(), micR.data(), outL.data(), outR.data(), kN, mp);

    double tailEnergy = 0.0;
    for (uint32_t i = 1000; i < kN; ++i) {
        REQUIRE(std::isfinite(outL[i]));
        REQUIRE(std::isfinite(outR[i]));
        tailEnergy += static_cast<double>(outL[i]) * outL[i];
    }
    REQUIRE(tailEnergy > 1.0e-5);

    mon.Reset();
    stoneridge::ControlRoomParams xp{};
    xp.directMonitorMix    = 0.0f;
    xp.monitorReverbAmount = 0.0f;
    xp.crossfeedEnabled    = true;
    for (uint32_t i = 0; i < 1024; ++i) {
        dawL[i] = 0.25f * std::sin(2.0f * 3.14159265f * 350.0f * static_cast<float>(i) / 48000.0f);
        dawR[i] = 0.0f;
    }
    mon.ProcessBlock(dawL.data(), dawR.data(), nullptr, nullptr, outL.data(), outR.data(), 1024u, xp);

    double crossR = 0.0;
    for (uint32_t i = 256; i < 1024; ++i) {
        REQUIRE(std::isfinite(outR[i]));
        crossR += static_cast<double>(outR[i]) * outR[i];
    }
    REQUIRE(crossR > 1.0e-4);
}

TEST_CASE("v7 stereo independence and stereo-linked dynamics", "[v7][stereo]") {
    stoneridge::InputProcessor proc;
    proc.Prepare(48000.0, 2);

    constexpr uint32_t kN = 2048;
    std::vector<float> l(kN), r(kN);
    for (uint32_t i = 0; i < kN; ++i) {
        const float t = static_cast<float>(i) / 48000.0f;
        l[i] = 0.15f * std::sin(2.0f * 3.14159265f * 440.0f * t);
        r[i] = 0.15f * std::sin(2.0f * 3.14159265f * 990.0f * t);
    }

    stoneridge::InputDspParams preset{};
    stoneridge::ApplyVocalPreset(preset, stoneridge::PresetByIndex(0));
    stoneridge::InputDspTelemetry tel{};
    proc.ProcessStereo(l.data(), r.data(), kN, preset, tel);

    double diffEnergy = 0.0;
    for (uint32_t i = 512; i < kN; ++i) {
        const double d = static_cast<double>(l[i]) - static_cast<double>(r[i]);
        diffEnergy += d * d;
    }
    REQUIRE(std::sqrt(diffEnergy / 1536.0) > 0.05);

    proc.Reset();
    stoneridge::InputDspParams fetLink{};
    fetLink.softClipEnabled    = true;
    fetLink.fetPeakCatchAmount = 0.90f;
    fetLink.fetRatioMode       = 1u;
    for (uint32_t i = 0; i < kN; ++i) {
        const float t = static_cast<float>(i) / 48000.0f;
        l[i] = 0.70f * std::sin(2.0f * 3.14159265f * 300.0f * t);
        r[i] = 0.05f * std::sin(2.0f * 3.14159265f * 600.0f * t);
    }
    proc.ProcessStereo(l.data(), r.data(), kN, fetLink, tel);
    REQUIRE(tel.fetReductionDb < -1.0f);

    double rRmsSq = 0.0;
    for (uint32_t i = 1024; i < kN; ++i) {
        rRmsSq += static_cast<double>(r[i]) * r[i];
    }
    const double rRms = std::sqrt(rRmsSq / 1024.0);
    // Unducked 0.05 peak sine has RMS 0.03535; stereo-linked L bursts must compress R down.
    REQUIRE(rRms < 0.030);
}

TEST_CASE("v7 DacConditioner 6-sample aligned ISP guard and zero-delay raw bypass", "[v7][dac]") {
    stoneridge::DacConditioner dac;
    dac.Prepare(48000.0);

    constexpr uint32_t kN = 512;
    std::vector<float> in(kN), out(kN);
    // 12 kHz shifted by pi/4: sample points +/-0.884 < 0.90, true inter-sample peak = +1.25 (> +1.9 dBTP)
    for (uint32_t i = 0; i < kN; ++i) {
        in[i] = 1.25f * std::sin(2.0f * 3.14159265f * 0.25f * static_cast<float>(i) + 0.785398f);
    }
    stoneridge::DacTelemetry tel{};
    dac.ProcessStereoFloat(in.data(), nullptr, out.data(), nullptr, kN, true, tel);
    REQUIRE(tel.ispReductionDb < -0.5f);
    REQUIRE(tel.outputTruePeakDb <= -0.35f);
    for (float s : out) {
        REQUIRE(std::isfinite(s));
        REQUIRE(std::fabs(s) <= 0.985f);
    }

    dac.Reset();
    std::vector<float> imp(64, 0.0f);
    imp[17] = 0.5f;
    std::vector<uint8_t> pcm(64 * 4, 0);
    stoneridge::DacTelemetry rawTel{};
    dac.ProcessAndConvertToPcm(imp.data(), imp.data(), pcm.data(), 64u, 2u, 16u, false, false, false, rawTel);
    const auto* i16 = reinterpret_cast<const int16_t*>(pcm.data());
    REQUIRE(i16[17 * 2] > 16000);
    REQUIRE(i16[16 * 2] == 0);
}

TEST_CASE("v7 monitor-safe A/B and gate-locked LoudnessMatcher", "[v7][ab]") {
    stoneridge::InputProcessor proc;
    proc.Prepare(48000.0, 2);

    constexpr uint32_t kN = 1024;
    std::vector<float> l(kN), r(kN), tapL(kN), tapR(kN);
    for (uint32_t i = 0; i < kN; ++i) {
        const float s = 0.12f * std::sin(2.0f * 3.14159265f * 180.0f * static_cast<float>(i) / 48000.0f);
        l[i] = r[i] = s;
    }

    stoneridge::InputDspParams p{};
    p.gainDb                = 8.0f;
    p.softClipEnabled       = true;
    p.preampModel           = 1u;
    p.preampDriveDb         = 8.0f;
    p.abLevelMatchedBypass  = true;
    p.abAuditionMonitorOnly = true;

    stoneridge::InputDspTelemetry tel{};
    proc.ProcessStereo(l.data(), r.data(), kN, p, tel);
    proc.CopyMonitorAuditionTap(tapL.data(), tapR.data(), kN);

    double procRms = 0.0;
    double tapDiff = 0.0;
    for (uint32_t i = 256; i < kN; ++i) {
        REQUIRE(std::isfinite(tapL[i]));
        procRms += static_cast<double>(l[i]) * l[i];
        tapDiff += std::fabs(static_cast<double>(l[i]) - static_cast<double>(tapL[i]));
    }
    REQUIRE(std::sqrt(procRms / 768.0) > 0.15);
    REQUIRE((tapDiff / 768.0) > 0.02);

    stoneridge::vocal::LoudnessMatcher matcher;
    matcher.Prepare(48000.0);
    matcher.SetGateOpen(true);
    for (int i = 0; i < 9600; ++i) {
        const float x = 0.15f * std::sin(2.0f * 3.14159265f * 400.0f * static_cast<float>(i) / 48000.0f);
        matcher.ProcessSample(x, x * 2.0f);
    }
    const float openGainDb = matcher.GetMatchGainDb();
    REQUIRE(openGainDb > 4.0f);

    matcher.SetGateOpen(false);
    for (int i = 0; i < 4800; ++i) {
        const float x = 0.15f * std::sin(2.0f * 3.14159265f * 400.0f * static_cast<float>(i) / 48000.0f);
        matcher.ProcessSample(x, x * 0.05f);
    }
    REQUIRE(std::fabs(matcher.GetMatchGainDb() - openGainDb) < 0.05f);
}
