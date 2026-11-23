#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "dsp/ControlRoomMonitor.h"
#include "dsp/DacConditioner.h"
#include "dsp/InputProcessor.h"
#include "engine/AdaptiveDriftResampler.h"
#include "engine/AudioFormatConvert.h"
#include "engine/RealtimeGuard.h"
#include "vocal/MicLocker.h"
#include "vocal/Oversampler.h"
#include "vocal/PreampAndConsole.h"
#include "vocal/VocalDynamics.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <vector>

namespace {

constexpr double kPi    = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;

// In-place Cooley-Tukey Radix-2 FFT (zero external dependencies)
void FftRadix2(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -kTwoPi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t j = 0; j < len / 2; ++j) {
                const auto u = a[i + j];
                const auto v = a[i + j + len / 2] * w;
                a[i + j]           = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

double MeasureGainDbAtFreq(stoneridge::InputProcessor& proc, const stoneridge::InputDspParams& params,
                           double freqHz, double sampleRate) {
    proc.Reset();
    constexpr uint32_t kTotal = 24000; // 0.5s so high-Q notch filters settle completely
    std::vector<float> l(kTotal), r(kTotal);
    for (uint32_t i = 0; i < kTotal; ++i) {
        const float s = 0.5f * static_cast<float>(std::sin(kTwoPi * freqHz * i / sampleRate));
        l[i] = r[i] = s;
    }
    stoneridge::InputDspTelemetry tel{};
    proc.ProcessStereo(l.data(), r.data(), kTotal, params, tel);

    // Measure steady-state RMS in final 25%
    double sumSq = 0.0;
    constexpr uint32_t kStart = (kTotal * 3) / 4;
    for (uint32_t i = kStart; i < kTotal; ++i) {
        sumSq += static_cast<double>(l[i]) * static_cast<double>(l[i]);
    }
    const double outRms = std::sqrt(sumSq / static_cast<double>(kTotal - kStart));
    const double inRms  = 0.5 / std::sqrt(2.0);
    return 20.0 * std::log10(std::max(outRms / inRms, 1e-10));
}

} // namespace

TEST_CASE("FFT Spectral Purity: THD+N, Twin-Tone IMD, and DC Offset", "[spectral]") {
    constexpr size_t kN = 4096;
    constexpr double kRate = 48000.0;
    constexpr size_t kBin = 85; // ~996.09 Hz coherent bin (zero spectral leakage!)
    const double freq = (kRate * kBin) / kN;

    // 1. 24-bit full-chain THD+N & DC Offset (with SoftClipper enabled below -1.5 dBFS knee)
    stoneridge::InputProcessor proc;
    proc.Prepare(kRate, 2);
    stoneridge::InputDspParams params{};
    params.gainDb          = 0.0f;
    params.softClipEnabled = true;
    params.audioHpfEnabled = false;
    params.gateEnabled     = false;

    // Pre-warm 16384 samples so the 5 Hz DC blocker settles completely before FFT capture
    std::vector<float> warmL(16384), warmR(16384);
    for (size_t i = 0; i < 16384; ++i) {
        warmL[i] = warmR[i] = 0.7f * static_cast<float>(std::sin(kTwoPi * freq * i / kRate));
    }
    stoneridge::InputDspTelemetry tel{};
    proc.ProcessStereo(warmL.data(), warmR.data(), 16384, params, tel);

    std::vector<float> l(kN), r(kN);
    for (size_t i = 0; i < kN; ++i) {
        l[i] = r[i] = 0.7f * static_cast<float>(std::sin(kTwoPi * freq * (i + 16384) / kRate)); // -3.1 dBFS (< -1.5 dBFS knee)
    }

    proc.ProcessStereo(l.data(), r.data(), static_cast<uint32_t>(kN), params, tel);

    std::vector<BYTE> pcm24(kN * 6);
    stoneridge::ConvertFloatToPcm(l.data(), r.data(), pcm24.data(), static_cast<uint32_t>(kN), 2, 24, false);
    stoneridge::ConvertPcmToFloat(pcm24.data(), l.data(), r.data(), static_cast<uint32_t>(kN), 2, 24, false);

    // Hann-windowed FFT
    std::vector<std::complex<double>> spec(kN);
    double dcSum = 0.0;
    for (size_t i = 0; i < kN; ++i) {
        dcSum += l[i];
        const double win = 0.5 * (1.0 - std::cos(kTwoPi * i / kN));
        spec[i] = std::complex<double>(l[i] * win, 0.0);
    }
    const double dcDb = 20.0 * std::log10(std::max(std::fabs(dcSum / kN), 1e-12));
    REQUIRE(dcDb < -110.0);

    FftRadix2(spec);

    double sigPow = 0.0, noisePow = 0.0;
    for (size_t b = 1; b < kN / 2; ++b) {
        const double p = std::norm(spec[b]);
        if (b >= kBin - 2 && b <= kBin + 2) sigPow += p;
        else noisePow += p;
    }
    const double thdnDb = 10.0 * std::log10(std::max(noisePow, 1e-24) / sigPow);
    REQUIRE(thdnDb < -125.0);
}

TEST_CASE("80 Hz Audio HPF Butterworth Bode Accuracy (-3 dB corner & -12 dB/oct slope)", "[spectral]") {
    stoneridge::InputProcessor proc;
    proc.Prepare(48000.0, 2);

    stoneridge::InputDspParams params{};
    params.gainDb          = 0.0f;
    params.softClipEnabled = false;
    params.audioHpfEnabled = true;
    params.gateEnabled     = false;

    const double g80   = MeasureGainDbAtFreq(proc, params, 80.0, 48000.0);
    const double g40   = MeasureGainDbAtFreq(proc, params, 40.0, 48000.0);
    const double g20   = MeasureGainDbAtFreq(proc, params, 20.0, 48000.0);
    const double g1000 = MeasureGainDbAtFreq(proc, params, 1000.0, 48000.0);

    REQUIRE(g80 == Catch::Approx(-3.01).margin(0.25));
    REQUIRE((g40 - g20) == Catch::Approx(12.0).margin(1.0)); // -12 dB/oct 2nd-order Butterworth slope
    REQUIRE(g1000 == Catch::Approx(0.0).margin(0.15));
}

TEST_CASE("Denormal Silence-Decay CPU Spike Audit (FTZ/DAZ verification)", "[spectral]") {
    stoneridge::InputProcessor::EnableDenormalProtection();
    stoneridge::InputProcessor proc;
    proc.Prepare(48000.0, 2);

    stoneridge::InputDspParams params{};
    params.gainDb          = 0.0f;
    params.softClipEnabled = true;
    params.audioHpfEnabled = true;
    params.gateEnabled     = true;
    params.gateThresholdDb = -30.0f;

    constexpr uint32_t kBlock = 128;
    constexpr uint32_t kIters = 2000;
    std::vector<float> normalL(kBlock, 0.25f), normalR(kBlock, -0.25f);
    std::vector<float> subL(kBlock, 1e-40f),   subR(kBlock, -1e-40f);
    stoneridge::InputDspTelemetry tel{};

    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kIters; ++i) {
        proc.ProcessStereo(normalL.data(), normalR.data(), kBlock, params, tel);
    }
    const auto t1 = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kIters; ++i) {
        proc.ProcessStereo(subL.data(), subR.data(), kBlock, params, tel);
    }
    const auto t2 = std::chrono::steady_clock::now();

    const double normalUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
    const double subUs    = std::chrono::duration<double, std::micro>(t2 - t1).count();

    // Without FTZ/DAZ, subnormals cause 10x-100x slowdown. With FTZ/DAZ, must stay < 2.0x.
    REQUIRE(subUs < normalUs * 2.0);
}

TEST_CASE("ADAA1 Soft-Clipper Aliasing Suppression vs. Memoryless Clipper", "[spectral]") {
    constexpr size_t kN = 4096;
    constexpr double kRate = 48000.0;
    constexpr size_t kBin = 939; // ~11,003.9 Hz (3rd harmonic 33 kHz folds to ~15 kHz; 5th 55 kHz folds to ~7 kHz!)
    const double freq = (kRate * kBin) / kN;

    stoneridge::InputProcessor proc;
    proc.Prepare(kRate, 2);
    stoneridge::InputDspParams params{};
    params.gainDb          = 12.0f; // Heavy +12 dB drive into soft-clipper
    params.softClipEnabled = true;

    // Pre-warm 2048 samples so 5 ms gain ramp & 5 Hz DC blocker settle before FFT
    std::vector<float> warmL(2048), warmR(2048);
    for (size_t i = 0; i < 2048; ++i) {
        warmL[i] = warmR[i] = 0.5f * static_cast<float>(std::sin(kTwoPi * freq * i / kRate));
    }
    stoneridge::InputDspTelemetry tel{};
    proc.ProcessStereo(warmL.data(), warmR.data(), 2048, params, tel);

    std::vector<float> adaaL(kN), adaaR(kN), naiveL(kN);
    for (size_t i = 0; i < kN; ++i) {
        const float x = 0.5f * static_cast<float>(std::sin(kTwoPi * freq * (i + 2048) / kRate));
        adaaL[i] = adaaR[i] = x;

        // Memoryless cubic reference driven +12 dB
        const float driven = x * 3.9810717f;
        constexpr float kKnee = 0.841395f;
        constexpr float kCeil = 0.966051f;
        constexpr float kSpan = (kCeil - kKnee) * 1.5f;
        const float a = std::fabs(driven);
        if (a <= kKnee) {
            naiveL[i] = driven;
        } else {
            const float sign = (driven >= 0.0f) ? 1.0f : -1.0f;
            const float t = a - kKnee;
            if (t >= kSpan) naiveL[i] = sign * kCeil;
            else naiveL[i] = sign * (kKnee + t - (t * t * t) / (3.0f * kSpan * kSpan));
        }
    }

    proc.ProcessStereo(adaaL.data(), adaaR.data(), static_cast<uint32_t>(kN), params, tel);

    auto measureAliasingPow = [&](const std::vector<float>& sig) {
        std::vector<std::complex<double>> spec(kN);
        for (size_t i = 0; i < kN; ++i) {
            const double w = 0.5 * (1.0 - std::cos(kTwoPi * i / kN));
            spec[i] = std::complex<double>(sig[i] * w, 0.0);
        }
        FftRadix2(spec);
        // Check aliased region away from fundamental (bins 50..850)
        double aliasPow = 0.0;
        for (size_t b = 50; b < 850; ++b) {
            aliasPow += std::norm(spec[b]);
        }
        return aliasPow;
    };

    const double adaaAlias  = measureAliasingPow(adaaL);
    const double naiveAlias = measureAliasingPow(naiveL);
    const double improvementDb = 10.0 * std::log10(naiveAlias / std::max(adaaAlias, 1e-20));
    REQUIRE(improvementDb >= 10.0);
}

TEST_CASE("60 Hz Mains Hum Notch, Room Noise Calibrator, Capture DSP & Dual Safety", "[spectral]") {
    stoneridge::InputProcessor proc;
    proc.Prepare(48000.0, 2);

    // 1. 60 Hz & 120 Hz Notch > 25 dB rejection, 1 kHz pass-through
    stoneridge::InputDspParams params{};
    params.softClipEnabled = false;
    params.humNotchFreqHz  = 60.0f;
    REQUIRE(MeasureGainDbAtFreq(proc, params, 60.0, 48000.0) < -25.0);
    REQUIRE(MeasureGainDbAtFreq(proc, params, 120.0, 48000.0) < -25.0);
    REQUIRE(MeasureGainDbAtFreq(proc, params, 1000.0, 48000.0) == Catch::Approx(0.0).margin(0.15));

    // 2. 2-Second Room Noise & 60 Hz Hum Auto-Calibrator
    params.humNotchFreqHz = 0.0f;
    proc.Reset();
    proc.StartLearnRoomNoise();
    std::vector<float> learnL(480), learnR(480);
    uint64_t sIdx = 0;
    for (int blk = 0; blk < 210; ++blk) {
        for (size_t i = 0; i < 480; ++i) {
            const float hum = 0.015f * static_cast<float>(std::sin(kTwoPi * 60.0 * static_cast<double>(sIdx++) / 48000.0));
            learnL[i] = learnR[i] = hum;
        }
        stoneridge::InputDspTelemetry tel{};
        proc.ProcessStereo(learnL.data(), learnR.data(), 480, params, tel);
    }
    const auto lr = proc.GetLearnResult();
    REQUIRE(lr.completed == true);
    REQUIRE(lr.recommendedHumHz == 60.0f);
    REQUIRE(lr.recommendedThreshDb > lr.noiseFloorDb);

    // 3. Dual-Track Safety Mode: L = processed (muted/gated), R = untouched raw capsule
    proc.Reset();
    params.gateEnabled         = false;
    params.audioHpfEnabled     = true;
    params.gainDb              = 12.0f;
    params.dualTrackSafetyMode = true;
    std::vector<float> bufL(512, 0.05f), bufR(512, 0.05f);
    for (size_t i = 0; i < 512; ++i) {
        const float v = 0.05f * static_cast<float>(std::sin(kTwoPi * 400.0 * i / 48000.0));
        bufL[i] = bufR[i] = v;
    }
    stoneridge::InputDspTelemetry tel{};
    proc.ProcessStereo(bufL.data(), bufR.data(), 512, params, tel);
    // Right channel remains untouched raw (~0.05 peak), Left is boosted +12 dB (~0.20 peak)
    float peakL = 0.0f, peakR = 0.0f;
    for (size_t i = 300; i < 512; ++i) {
        peakL = std::max(peakL, std::fabs(bufL[i]));
        peakR = std::max(peakR, std::fabs(bufR[i]));
    }
    REQUIRE(peakR < 0.06f);
    REQUIRE(peakL > 0.14f);

    // 4. 1.0 ms Look-Ahead Gate + De-Esser + Plosive Guard smoke & attenuation check
    proc.Reset();
    params.dualTrackSafetyMode  = false;
    params.gainDb               = 0.0f;
    params.gateEnabled          = true;
    params.gateLookAheadEnabled = true;
    params.gateThresholdDb      = -30.0f;
    params.deEsserAmount        = 0.8f;
    params.plosiveGuardEnabled  = true;
    params.vocalLevelerAmount   = 0.6f;

    std::vector<float> sibL(2048), sibR(2048);
    for (size_t i = 0; i < 2048; ++i) {
        const float s = 0.4f * static_cast<float>(std::sin(kTwoPi * 6500.0 * i / 48000.0));
        sibL[i] = sibR[i] = s;
    }
    proc.ProcessStereo(sibL.data(), sibR.data(), 2048, params, tel);
    REQUIRE(tel.deEsserReductionDb < -2.0f);

    // 5. Opto Vocal Leveler independent check (without de-esser pre-attenuating signal)
    params.deEsserAmount = 0.0f;
    std::vector<float> vocL(12000), vocR(12000);
    for (size_t i = 0; i < 12000; ++i) {
        const float v = 0.5f * static_cast<float>(std::sin(kTwoPi * 400.0 * i / 48000.0));
        vocL[i] = vocR[i] = v;
    }
    proc.ProcessStereo(vocL.data(), vocR.data(), 12000, params, tel);
    REQUIRE(tel.levelerReductionDb < -1.0f);
}

TEST_CASE("DacConditioner ITU-R BS.1770-4 True-Peak and ISP Suppression", "[dsp][dac][isp]") {
    stoneridge::DacConditioner rawMeter;
    stoneridge::DacConditioner guard;
    stoneridge::DacConditioner postMeter;
    rawMeter.Prepare(48000.0);
    guard.Prepare(48000.0);
    postMeter.Prepare(48000.0);

    // Classic fs/4 inter-sample peak stress pattern: [+0.99, +0.99, -0.99, -0.99] (~+2.9 dBTP)
    constexpr uint32_t N = 1024;
    std::vector<float> inL(N), inR(N), rawOutL(N), rawOutR(N), cleanL(N), cleanR(N);
    for (uint32_t i = 0; i < N; ++i) {
        const float v = ((i & 2u) == 0u) ? 0.99f : -0.99f;
        inL[i] = v;
        inR[i] = v;
    }

    stoneridge::DacTelemetry rawTel{};
    rawMeter.ProcessStereoFloat(inL.data(), inR.data(), rawOutL.data(), rawOutR.data(), N, false, rawTel);
    REQUIRE(rawTel.outputTruePeakDb > 2.5f);

    stoneridge::DacTelemetry guardTel{};
    guard.ProcessStereoFloat(inL.data(), inR.data(), cleanL.data(), cleanR.data(), N, true, guardTel);
    REQUIRE(guardTel.ispReductionDb < -2.0f);

    // Independent continuous BS.1770-4 check (warm up FIR across [0..256), measure steady [256..N))
    float steadyTpLin = 0.0f;
    for (uint32_t i = 0; i < N; ++i) {
        const float tp = postMeter.PushAndMeasureTruePeakSample(cleanL[i], cleanR[i]);
        if (i >= 256) steadyTpLin = std::max(steadyTpLin, tp);
    }
    const float steadyTpDb = 20.0f * std::log10(std::max(steadyTpLin, 1e-5f));
    REQUIRE(steadyTpDb <= -0.45f);
}

TEST_CASE("96 kHz 16-bit Psychoacoustic Noise Shaping & 24-bit TPDF Dither", "[dsp][dac][shaper]") {
    constexpr double kFs = 96000.0;
    constexpr size_t kTotal = 32768;
    constexpr size_t kFftN  = 16384;

    // -60 dBFS ~1 kHz coherent pure tone (exact integer bin prevents Hann leakage below -145 dBFS)
    constexpr size_t kToneBin = 171;
    const double toneHz = (kFs * static_cast<double>(kToneBin)) / static_cast<double>(kFftN);
    std::vector<float> inL(kTotal), inR(kTotal);
    const double amp = 0.001; // -60 dBFS
    for (size_t i = 0; i < kTotal; ++i) {
        const float s = static_cast<float>(amp * std::sin(kTwoPi * toneHz * i / kFs));
        inL[i] = s;
        inR[i] = s;
    }

    auto measureBandNoiseDb = [&](const std::vector<float>& pcmFloat, double maxFreqHz, double& outMaxSpurDb) {
        std::vector<std::complex<double>> fft(kFftN);
        double winSq = 0.0;
        for (size_t i = 0; i < kFftN; ++i) {
            const double w = 0.5 * (1.0 - std::cos(kTwoPi * i / kFftN));
            winSq += w * w;
            fft[i] = std::complex<double>(pcmFloat[kTotal - kFftN + i] * w, 0.0);
        }
        FftRadix2(fft);

        const size_t fundBin = kToneBin;
        const size_t maxAudioBin = static_cast<size_t>(maxFreqHz * kFftN / kFs);
        double noiseEnergy = 0.0;
        double maxSpurLin  = 0.0;
        double winSum      = 0.0;
        for (size_t i = 0; i < kFftN; ++i) {
            winSum += 0.5 * (1.0 - std::cos(kTwoPi * i / kFftN));
        }
        for (size_t k = 4; k <= maxAudioBin; ++k) {
            if (std::abs(static_cast<long>(k) - static_cast<long>(fundBin)) <= 4) continue;
            const double mag = std::abs(fft[k]);
            noiseEnergy += mag * mag;
            maxSpurLin = std::max(maxSpurLin, (2.0 * mag) / winSum);
        }
        outMaxSpurDb = 20.0 * std::log10(std::max(maxSpurLin, 1e-12));
        const double noiseRms = std::sqrt((2.0 * noiseEnergy) / (static_cast<double>(kFftN) * winSq));
        return 20.0 * std::log10(std::max(noiseRms, 1e-12));
    };

    // 1. Raw un-dithered 16-bit truncation/rounding (full coherent harmonic distortion spurs)
    std::vector<BYTE> rawPcm(kTotal * 4);
    stoneridge::DacConditioner rawCond;
    stoneridge::DacTelemetry tel{};
    rawCond.Prepare(kFs);
    rawCond.ProcessAndConvertToPcm(inL.data(), inR.data(), rawPcm.data(),
                                   static_cast<uint32_t>(kTotal), 2, 16, false, false, false, tel);
    std::vector<float> rawRecL(kTotal), rawRecR(kTotal);
    stoneridge::ConvertPcmToFloat(rawPcm.data(), rawRecL.data(), rawRecR.data(),
                                  static_cast<uint32_t>(kTotal), 2, 16, false);
    double rawSpurDb = 0.0;
    const double rawNoise10kDb = measureBandNoiseDb(rawRecL, 10000.0, rawSpurDb);

    // 2. 96 kHz 3rd-order psychoacoustic noise-shaped 16-bit conversion
    std::vector<BYTE> nsPcm(kTotal * 4);
    stoneridge::DacConditioner nsCond;
    nsCond.Prepare(kFs);
    nsCond.ProcessAndConvertToPcm(inL.data(), inR.data(), nsPcm.data(),
                                  static_cast<uint32_t>(kTotal), 2, 16, false, false, true, tel);
    std::vector<float> nsRecL(kTotal), nsRecR(kTotal);
    stoneridge::ConvertPcmToFloat(nsPcm.data(), nsRecL.data(), nsRecR.data(),
                                  static_cast<uint32_t>(kTotal), 2, 16, false);
    double nsSpur18kDb = 0.0, nsSpur10kDb = 0.0;
    const double nsNoise18kDb = measureBandNoiseDb(nsRecL, 18000.0, nsSpur18kDb);
    const double nsNoise10kDb = measureBandNoiseDb(nsRecL, 10000.0, nsSpur10kDb);

    REQUIRE(nsNoise18kDb < -108.0);
    REQUIRE(nsNoise10kDb < -111.0);
    REQUIRE((rawNoise10kDb - nsNoise10kDb) > 3.0);
    REQUIRE(nsSpur10kDb < rawSpurDb);
    (void)nsSpur18kDb;

    // 2b. Low-level fade/reverb linearization (-84 dBFS ~ +/-2 LSBs): TPDF + NS eliminates odd harmonic spikes
    std::vector<float> lowL(kTotal), lowR(kTotal);
    for (size_t i = 0; i < kTotal; ++i) {
        const float s = static_cast<float>(6.30957e-5 * std::sin(kTwoPi * toneHz * i / kFs));
        lowL[i] = lowR[i] = s;
    }
    rawCond.Reset();
    rawCond.ProcessAndConvertToPcm(lowL.data(), lowR.data(), rawPcm.data(),
                                   static_cast<uint32_t>(kTotal), 2, 16, false, false, false, tel);
    stoneridge::ConvertPcmToFloat(rawPcm.data(), rawRecL.data(), rawRecR.data(),
                                  static_cast<uint32_t>(kTotal), 2, 16, false);
    double lowRawSpurDb = 0.0;
    measureBandNoiseDb(rawRecL, 18000.0, lowRawSpurDb);

    nsCond.Reset();
    nsCond.ProcessAndConvertToPcm(lowL.data(), lowR.data(), nsPcm.data(),
                                  static_cast<uint32_t>(kTotal), 2, 16, false, false, true, tel);
    stoneridge::ConvertPcmToFloat(nsPcm.data(), nsRecL.data(), nsRecR.data(),
                                  static_cast<uint32_t>(kTotal), 2, 16, false);
    double lowNsSpurDb = 0.0;
    measureBandNoiseDb(nsRecL, 18000.0, lowNsSpurDb);
    REQUIRE((lowRawSpurDb - lowNsSpurDb) > 12.0);

    // 3. 24-bit TPDF dither at 48 kHz leaves 20Hz-18kHz noise floor < -135 dBFS
    std::vector<BYTE> pcm24(kTotal * 6);
    stoneridge::DacConditioner d24;
    d24.Prepare(48000.0);
    d24.ProcessAndConvertToPcm(inL.data(), inR.data(), pcm24.data(),
                               static_cast<uint32_t>(kTotal), 2, 24, false, false, true, tel);
    std::vector<float> rec24L(kTotal), rec24R(kTotal);
    stoneridge::ConvertPcmToFloat(pcm24.data(), rec24L.data(), rec24R.data(),
                                  static_cast<uint32_t>(kTotal), 2, 24, false);
    double dummySpur = 0.0;
    REQUIRE(measureBandNoiseDb(rec24L, 18000.0, dummySpur) < -135.0);
}

TEST_CASE("96 kHz 22 kHz Ultrasonic ADC LPF & Single-Path Mono Parity", "[dsp][ultrasonic][mono]") {
    stoneridge::InputProcessor proc;
    proc.Prepare(96000.0, 2);

    stoneridge::InputDspParams params{};
    params.softClipEnabled      = false;
    params.ultrasonicLpfEnabled = true;

    auto measureToneRms = [&](double freqHz) {
        proc.Reset();
        std::vector<float> l(4096), r(4096);
        for (size_t i = 0; i < 4096; ++i) {
            const float s = 0.25f * static_cast<float>(std::sin(kTwoPi * freqHz * i / 96000.0));
            l[i] = r[i] = s;
        }
        stoneridge::InputDspTelemetry tel{};
        proc.ProcessStereo(l.data(), r.data(), 4096, params, tel);

        // Single-path mono invariant: L == R bit-identically when dualTrackSafetyMode == false
        for (size_t i = 0; i < 4096; ++i) {
            REQUIRE(l[i] == r[i]);
        }

        double sumSq = 0.0;
        for (size_t i = 2048; i < 4096; ++i) {
            sumSq += static_cast<double>(l[i]) * static_cast<double>(l[i]);
        }
        return std::sqrt(sumSq / 2048.0);
    };

    const double rms1k  = measureToneRms(1000.0);
    const double rms38k = measureToneRms(38000.0);
    const double attenDb = 20.0 * std::log10(std::max(rms1k, 1e-12) / std::max(rms38k, 1e-12));

    REQUIRE(rms1k > 0.17);
    REQUIRE(attenDb > 20.0);
}

TEST_CASE("AdaptiveDriftResampler Clock-Drift Lock (+120 ppm and -120 ppm)", "[engine][asrc]") {
    constexpr double kFs = 48000.0;
    constexpr uint32_t kBlock = 128;
    constexpr size_t kFftN = 16384;

    for (double driftPpm : { 120.0, -120.0 }) {
        stoneridge::LockFreeRingBuffer<float> ringL, ringR;
        ringL.Resize(8192);
        ringR.Resize(8192);

        stoneridge::AdaptiveDriftResampler asrc;
        asrc.Prepare(kFs, kBlock);
        const double targetFill = asrc.GetTargetFill();

        const double rateRatio = 1.0 + driftPpm * 1.0e-6;
        double inPhaseAccum = 0.0;
        uint64_t inSampleIdx = 0;

        auto pushNextIn = [&](size_t count) {
            std::vector<float> tmp(count);
            for (size_t i = 0; i < count; ++i) {
                tmp[i] = 0.5f * static_cast<float>(std::sin(kTwoPi * 1000.0 * static_cast<double>(inSampleIdx++) / kFs));
            }
            ringL.Push(tmp.data(), count);
            ringR.Push(tmp.data(), count);
        };

        // Pre-seed at nominal targetFill
        pushNextIn(static_cast<size_t>(targetFill));

        std::vector<float> outL(kBlock), outR(kBlock);
        std::vector<float> recorded;
        recorded.reserve(kFftN + kBlock);

        uint32_t underruns = 0;
        const uint32_t totalBlocks = 1800; // ~4.8 seconds at 48 kHz / 128 smp
        for (uint32_t blk = 0; blk < totalBlocks; ++blk) {
            inPhaseAccum += static_cast<double>(kBlock) * rateRatio;
            const size_t pushCnt = static_cast<size_t>(std::floor(inPhaseAccum));
            inPhaseAccum -= static_cast<double>(pushCnt);
            pushNextIn(pushCnt);

            bool underrun = false;
            asrc.Process(ringL, ringR, outL.data(), outR.data(), kBlock, true, false, underrun);
            if (underrun) ++underruns;

            if (blk >= totalBlocks - 160) {
                recorded.insert(recorded.end(), outL.begin(), outL.end());
            }
        }

        const double finalFill = static_cast<double>(ringL.AvailableToRead());
        REQUIRE(underruns == 0);
        REQUIRE(std::fabs(finalFill - targetFill) < static_cast<double>(kBlock));

        // Verify FFT THD+N < -110 dB using Hann^3 window (-42 dB/oct sidelobe decay; <-150 dB past +/-12 bins)
        REQUIRE(recorded.size() >= kFftN);
        std::vector<std::complex<double>> fft(kFftN);
        for (size_t i = 0; i < kFftN; ++i) {
            const double h = 0.5 * (1.0 - std::cos((kTwoPi * i) / kFftN));
            const double w = h * h * h;
            fft[i] = std::complex<double>(recorded[i] * w, 0.0);
        }
        FftRadix2(fft);

        size_t peakBin = 1;
        double peakMag = 0.0;
        for (size_t k = 10; k < kFftN / 2; ++k) {
            const double m = std::abs(fft[k]);
            if (m > peakMag) {
                peakMag = m;
                peakBin = k;
            }
        }
        double noisePower = 0.0;
        for (size_t k = 12; k < kFftN / 2; ++k) {
            if (std::abs(static_cast<long>(k) - static_cast<long>(peakBin)) <= 12) continue;
            const double m = std::abs(fft[k]);
            noisePower += m * m;
        }
        const double thdnDb = 10.0 * std::log10(std::max(noisePower, 1e-18) / (peakMag * peakMag));
        REQUIRE(thdnDb < -110.0);
    }
}

// ==============================================================================
// 11. Phase 1: 3-Way Mic 2 Stem Routing, K-Weighted A/B Loudness Match & Null Test
// ==============================================================================
TEST_CASE("Phase 1: 3-Way Mic 2 stem routing and K-weighted BS.1770 A/B loudness match", "[phase1][dsp]") {
    constexpr double kFs = 48000.0;
    constexpr uint32_t kN = 24000; // 500 ms

    stoneridge::InputProcessor procA;
    stoneridge::InputProcessor procB;
    procA.Prepare(kFs, 2);
    procB.Prepare(kFs, 2);

    std::vector<float> l1(kN), r1(kN), l2(kN), r2(kN);
    for (uint32_t i = 0; i < kN; ++i) {
        const float s = 0.12f * std::sin(static_cast<float>(kTwoPi * 440.0 * i / kFs))
                      + 0.10f * std::sin(static_cast<float>(kTwoPi * 6500.0 * i / kFs));
        l1[i] = r1[i] = l2[i] = r2[i] = s;
    }

    stoneridge::InputDspParams p{};
    p.gainDb        = 8.0f;
    p.deEsserAmount = 0.85f;
    p.mic2RoutingMode = 1u; // Pre-Dynamics Stem on Mic 2
    stoneridge::InputDspTelemetry tel1{}, tel2{};

    {
        stoneridge::ScopedRealtimeThread rtGuard;
        procA.ProcessStereo(l1.data(), r1.data(), kN, p, tel1);
    }

    // Mic 2 (Pre-Dynamics Stem) must have full +8 dB gain (unattenuated by De-Esser),
    // while Mic 1 (Master Vocal) is attenuated by the De-Esser (> 2.5 dB GR).
    double energyMic1 = 0.0, energyMic2 = 0.0;
    for (uint32_t i = kN / 2; i < kN; ++i) {
        energyMic1 += static_cast<double>(l1[i]) * l1[i];
        energyMic2 += static_cast<double>(r1[i]) * r1[i];
    }
    REQUIRE(tel1.deEsserReductionDb < -2.0f);
    REQUIRE(energyMic2 > energyMic1 * 1.35);

    // Now enable K-weighted level-matched A/B bypass on procB after warming up procB
    p.abLevelMatchedBypass = false;
    procB.ProcessStereo(l2.data(), r2.data(), kN, p, tel2);
    REQUIRE(std::fabs(tel2.abMatchGainDb) > 1.5f); // Must detect the +8 dB - DeEsser loudness shift

    // Re-fill and compare A (processed) vs B (level-matched raw) in the second 500 ms window
    for (uint32_t i = 0; i < kN; ++i) {
        const float s = 0.12f * std::sin(static_cast<float>(kTwoPi * 440.0 * i / kFs))
                      + 0.10f * std::sin(static_cast<float>(kTwoPi * 6500.0 * i / kFs));
        l1[i] = r1[i] = l2[i] = r2[i] = s;
    }
    p.abLevelMatchedBypass = false;
    procA.ProcessStereo(l1.data(), r1.data(), kN, p, tel1);
    p.abLevelMatchedBypass = true;
    p.mic2RoutingMode = 2u; // Raw safety backup on Mic 2
    procB.ProcessStereo(l2.data(), r2.data(), kN, p, tel2);

    // Verify K-weighted A/B loudness matches within +/- 0.6 dB and Mic 2 is untouched raw (~0.11 RMS)
    stoneridge::vocal::LoudnessMatcher verifier;
    verifier.Prepare(kFs);
    float lastRatioDb = 99.0f;
    double rawRmsSq = 0.0;
    for (uint32_t i = 0; i < kN; ++i) {
        verifier.ProcessSample(l2[i], l1[i]);
        rawRmsSq += static_cast<double>(r2[i]) * r2[i];
    }
    lastRatioDb = verifier.GetMatchGainDb();
    REQUIRE(std::fabs(lastRatioDb) < 0.6f);
    const double rawRms = std::sqrt(rawRmsSq / kN);
    REQUIRE(rawRms == Catch::Approx(0.11045).margin(0.005));
}

// ==============================================================================
// 12. Phase 1: Polyphase IIR 2x/4x Oversampler Passband & Aliasing Suppression
// ==============================================================================
TEST_CASE("Phase 1: Polyphase IIR 4x Oversampler suppresses nonlinear waveshaper aliasing", "[phase1][oversampler]") {
    constexpr double kFs = 48000.0;
    constexpr uint32_t kBlock = 256;
    constexpr uint32_t kBlocks = 32;
    constexpr uint32_t kTotal = kBlock * kBlocks; // 8192

    stoneridge::vocal::Oversampler os4;
    os4.Prepare(4);

    // High-frequency test tone (10.5 kHz -> 3rd harmonic at 31.5 kHz folds back to 16.5 kHz at 1x)
    const double f0 = (1792.0 * kFs) / kTotal; // Exact FFT bin 1792 = 10500 Hz
    std::vector<float> in(kTotal), out1x(kTotal), out4x(kTotal);
    for (uint32_t i = 0; i < kTotal; ++i) {
        in[i] = 0.85f * static_cast<float>(std::sin(kTwoPi * f0 * i / kFs));
    }

    auto cubicClip = [](float x) noexcept -> float {
        const float u = std::clamp(x * 1.6f, -1.5f, 1.5f);
        return u - (u * u * u) / 6.75f;
    };

    for (uint32_t i = 0; i < kTotal; ++i) {
        out1x[i] = cubicClip(in[i]);
    }

    for (uint32_t b = 0; b < kBlocks; ++b) {
        const uint32_t off = b * kBlock;
        float* hi = os4.Upsample(in.data() + off, kBlock);
        for (uint32_t k = 0; k < kBlock * 4; ++k) {
            hi[k] = cubicClip(hi[k]);
        }
        os4.Downsample(out4x.data() + off, kBlock);
    }

    // Measure aliased 3rd-harmonic foldback bin: |48000 - 3*10500| = 16500 Hz -> bin 2816
    constexpr size_t kN = 4096;
    constexpr size_t kOff = 4096;
    std::vector<std::complex<double>> fft1(kN), fft4(kN);
    for (size_t i = 0; i < kN; ++i) {
        const double w = 0.5 * (1.0 - std::cos((kTwoPi * i) / kN));
        fft1[i] = std::complex<double>(out1x[kOff + i] * w, 0.0);
        fft4[i] = std::complex<double>(out4x[kOff + i] * w, 0.0);
    }
    FftRadix2(fft1);
    FftRadix2(fft4);

    const size_t fundBin  = 896;  // 10500 Hz in 4096-pt FFT
    const size_t aliasBin = 1408; // 16500 Hz aliased 3rd harmonic in 4096-pt FFT
    const double alias1xDb = 20.0 * std::log10(std::abs(fft1[aliasBin]) / std::abs(fft1[fundBin]));
    const double alias4xDb = 20.0 * std::log10(std::abs(fft4[aliasBin]) / std::abs(fft4[fundBin]));

    // 4x oversampling must suppress the 16.5 kHz aliased foldback by > 45 dB vs 1x
    REQUIRE(alias1xDb > -30.0);
    REQUIRE(alias4xDb < alias1xDb - 45.0);
}

// ==============================================================================
// 13. Phase 1: ControlRoomMonitor Dry-ASIO Isolation, Comfort Chamber & Cue Carve
// ==============================================================================
TEST_CASE("Phase 1: ControlRoomMonitor adds monitor-only reverb, crossfeed, and anti-bleed cue carve", "[phase1][monitor]") {
    constexpr double kFs = 48000.0;
    constexpr uint32_t kN = 4096;

    stoneridge::ControlRoomMonitor mon;
    mon.Prepare(kFs);

    // 1. Comfort Chamber: feed a 200-sample vocal burst into micL/R with dawL/R = 0
    std::vector<float> dawL(kN, 0.0f), dawR(kN, 0.0f);
    std::vector<float> micL(kN, 0.0f), micR(kN, 0.0f);
    std::vector<float> outL(kN, 0.0f), outR(kN, 0.0f);
    for (uint32_t i = 0; i < 200; ++i) {
        micL[i] = micR[i] = 0.5f * std::sin(static_cast<float>(kTwoPi * 440.0 * i / kFs));
    }

    stoneridge::ControlRoomParams cp{};
    cp.directMonitorMix    = 0.8f;
    cp.monitorReverbAmount = 0.6f;
    cp.crossfeedEnabled    = false;
    cp.cueCarveEnabled     = false;

    mon.ProcessBlock(dawL.data(), dawR.data(), micL.data(), micR.data(), outL.data(), outR.data(), kN, cp);

    // Verify FDN reverb tail is present well after the 200-sample dry burst ends (samples 800..3000),
    // while micL/R (ASIO recorded input buffers) remain 100% zero after sample 200!
    double tailEnergy = 0.0, micTailEnergy = 0.0;
    for (uint32_t i = 800; i < 3000; ++i) {
        tailEnergy    += static_cast<double>(outL[i]) * outL[i] + static_cast<double>(outR[i]) * outR[i];
        micTailEnergy += static_cast<double>(micL[i]) * micL[i];
    }
    REQUIRE(micTailEnergy == 0.0);
    REQUIRE(tailEnergy > 1e-3);

    // 2. Anti-Bleed Dynamic Cue Carve: 2.2 kHz backing track is ducked when singing
    mon.Reset();
    for (uint32_t i = 0; i < kN; ++i) {
        dawL[i] = dawR[i] = 0.4f * std::sin(static_cast<float>(kTwoPi * 2200.0 * i / kFs));
        micL[i] = micR[i] = 0.0f;
    }
    cp.directMonitorMix    = 0.002f; // Negligible direct blend so we measure backing track energy directly
    cp.monitorReverbAmount = 0.0f;
    cp.cueCarveEnabled     = true;
    mon.ProcessBlock(dawL.data(), dawR.data(), micL.data(), micR.data(), outL.data(), outR.data(), kN, cp);
    double backingNoVoice = 0.0;
    for (uint32_t i = kN / 2; i < kN; ++i) backingNoVoice += static_cast<double>(outL[i]) * outL[i];

    mon.Reset();
    for (uint32_t i = 0; i < kN; ++i) {
        micL[i] = micR[i] = 0.35f; // Active vocal envelope
    }
    mon.ProcessBlock(dawL.data(), dawR.data(), micL.data(), micR.data(), outL.data(), outR.data(), kN, cp);
    double backingWithVoice = 0.0;
    for (uint32_t i = kN / 2; i < kN; ++i) {
        const double pureBack = outL[i] - micL[i] * cp.directMonitorMix;
        backingWithVoice += pureBack * pureBack;
    }
    const double duckDb = 10.0 * std::log10(backingWithVoice / backingNoVoice);
    REQUIRE(duckDb < -1.2);
    REQUIRE(duckDb > -2.5);
}

// ==============================================================================
// 14. Phase 2: Serial 1176 FET Peak Catcher -> LA-2A T4B Dual-Memory Opto ODE
// ==============================================================================
TEST_CASE("Phase 2: 1176 FET peak catcher and LA-2A T4B dual-memory CdS release hysteresis", "[phase2][dsp]") {
    constexpr double kFs = 48000.0;

    // 1. 1176 FET catches fast 1 ms transient spikes (< 1 ms attack) with zero RT allocs
    stoneridge::vocal::Fet1176Stage fet;
    fet.Prepare(kFs);
    std::vector<float> burst(2400, 0.0f); // 50 ms
    for (uint32_t i = 0; i < 480; ++i) { // 10 ms loud spike (-1 dBFS)
        burst[i] = 0.89f * std::sin(static_cast<float>(kTwoPi * 1000.0 * i / kFs));
    }
    {
        stoneridge::ScopedRealtimeThread rtGuard;
        const float gr = fet.ProcessBlock(burst.data(), static_cast<uint32_t>(burst.size()), 0.85f, 1u);
        REQUIRE(gr < -3.0f);
    }

    // 2. LA-2A T4B Dual-Memory CdS Hysteresis:
    //    Sustained 2.5s vocal chorus must charge gSlow much deeper -> slower release
    //    than a short 100ms vocal burst at the exact same amplitude!
    auto measureRecoveryGainAfter = [&](uint32_t loudFrames) -> float {
        stoneridge::vocal::OptoLa2aT4bStage opto;
        opto.Prepare(kFs);
        std::vector<float> loud(loudFrames);
        for (uint32_t i = 0; i < loudFrames; ++i) {
            loud[i] = 0.50f * std::sin(static_cast<float>(kTwoPi * 330.0 * i / kFs));
        }
        opto.ProcessBlock(loud.data(), loudFrames, 0.85f);

        // Now feed 200 ms of quiet tail (0.01 = -40 dBFS) and inspect gain at the end
        constexpr uint32_t kTail = 9600;
        std::vector<float> tail(kTail, 0.01f);
        std::vector<float> trace(kTail, 1.0f);
        opto.ProcessBlock(tail.data(), kTail, 0.85f, trace.data());
        return trace.back();
    };

    const float recAfterShort = measureRecoveryGainAfter(4800);   // 100 ms burst
    const float recAfterLong  = measureRecoveryGainAfter(120000); // 2.5 s sustained chorus
    REQUIRE(recAfterShort > recAfterLong + 0.05f);
}

// ==============================================================================
// 15. Phase 3: Test V6 Exact Langevin, Test V4 Preamp THD, Test V3 Aliasing & Pultec EQ
// ==============================================================================
TEST_CASE("Phase 3: Exact Langevin V6, Jiles-Atherton THD trend V4, 4x Preamp aliasing V3 & Pultec EQP-1A", "[phase3][dsp]") {
    // 1. Test V6: Exact Langevin(x) matches long-double coth(x) - 1/x within < 1e-6 across [-50, 50]
    for (int i = -500; i <= 500; ++i) {
        const double x = static_cast<double>(i) * 0.1;
        const double approx = stoneridge::vocal::LangevinExact(x);
        long double exact = 0.0L;
        if (std::fabs(x) < 1e-6) {
            exact = 0.0L;
        } else {
            const long double lx = static_cast<long double>(x);
            exact = (1.0L / std::tanh(lx)) - (1.0L / lx);
        }
        REQUIRE(std::fabs(approx - static_cast<double>(exact)) < 1e-6);
    }

    // 2. Test V4: Preamp THD(50 Hz) > THD(1 kHz) on Neve 1073 JA Iron, and SSL 9000J THD < 0.01%
    constexpr double kFs = 48000.0;
    auto measureThd = [&](uint32_t model, float driveDb, float iron, double freqHz) -> double {
        stoneridge::vocal::PreampStage pre;
        pre.Prepare(kFs);
        constexpr size_t kN = 4096;
        // Snap freqHz to exact FFT bin to prevent spectral leakage
        const double binFreq = std::round(freqHz * kN / kFs) * kFs / kN;
        std::vector<float> buf(kN * 2);
        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = 0.45f * static_cast<float>(std::sin(kTwoPi * binFreq * i / kFs));
        }
        {
            stoneridge::ScopedRealtimeThread rtGuard;
            pre.ProcessBlock(buf.data(), static_cast<uint32_t>(buf.size()), model, driveDb, iron);
        }
        std::vector<std::complex<double>> fft(kN);
        for (size_t i = 0; i < kN; ++i) {
            const double w = 0.5 * (1.0 - std::cos((kTwoPi * i) / kN));
            fft[i] = std::complex<double>(buf[kN + i] * w, 0.0);
        }
        FftRadix2(fft);
        const size_t fundBin = static_cast<size_t>(std::round(binFreq * kN / kFs));
        const double fundPow = std::norm(fft[fundBin]);
        double harmPow = 0.0;
        for (size_t h = 2; h <= 7; ++h) {
            const size_t hb = fundBin * h;
            if (hb + 1 < kN / 2) {
                harmPow += std::norm(fft[hb]) + std::norm(fft[hb - 1]) + std::norm(fft[hb + 1]);
            }
        }
        return std::sqrt(harmPow / std::max(fundPow, 1e-18));
    };

    const double neve50  = measureThd(static_cast<uint32_t>(stoneridge::vocal::PreampModel::Neve1073), 9.0f, 1.0f, 50.0);
    const double neve1k  = measureThd(static_cast<uint32_t>(stoneridge::vocal::PreampModel::Neve1073), 9.0f, 1.0f, 1000.0);
    const double ssl1k   = measureThd(static_cast<uint32_t>(stoneridge::vocal::PreampModel::Ssl9000J), 0.0f, 0.0f, 1000.0);
    REQUIRE(neve50 > neve1k * 1.15);
    REQUIRE(neve50 > 0.002);      // Measurable magnetic hysteresis harmonics
    REQUIRE(ssl1k < 0.002);       // SSL 9000J ultra-clean < 0.2% even at 0.45 amplitude

    // 3. Test V3: High-frequency aliasing on 11 kHz sine at +12 dB drive
    {
        stoneridge::vocal::PreampStage pre;
        pre.Prepare(kFs);
        constexpr size_t kN = 4096;
        const size_t fundBin = 939; // ~11003.9 Hz
        const double f0 = fundBin * kFs / kN;
        std::vector<float> buf(kN * 2);
        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = 0.50f * static_cast<float>(std::sin(kTwoPi * f0 * i / kFs));
        }
        pre.ProcessBlock(buf.data(), static_cast<uint32_t>(buf.size()),
                         static_cast<uint32_t>(stoneridge::vocal::PreampModel::TelefunkenV76), 12.0f, 0.8f);
        std::vector<std::complex<double>> fft(kN);
        double maxSig = 0.0;
        for (size_t i = 0; i < kN; ++i) {
            const double w = 0.5 * (1.0 - std::cos((kTwoPi * i) / kN));
            fft[i] = std::complex<double>(buf[kN + i] * w, 0.0);
            maxSig = std::max<double>(maxSig, std::abs(buf[kN + i]));
        }
        FftRadix2(fft);
        // Check true aliased foldback harmonics (3rd @ 15 kHz, 4th @ 4 kHz, 5th @ 7 kHz)
        // skipping the valid in-band fundamental (11 kHz) and valid in-band 2nd harmonic (22 kHz).
        const double fundMag = std::abs(fft[fundBin]);
        double aliasPower = 0.0;
        const size_t aliasBins[3] = {
            static_cast<size_t>(kN - 3 * fundBin), // |48k - 33k| = 15 kHz
            static_cast<size_t>(kN - 4 * fundBin), // |48k - 44k| =  4 kHz
            static_cast<size_t>(5 * fundBin - kN)  // |55k - 48k| =  7 kHz
        };
        for (size_t b : aliasBins) {
            for (int d = -2; d <= 2; ++d) {
                aliasPower += std::norm(fft[b + d]);
            }
        }
        const double aliasDb = 10.0 * std::log10(std::max(aliasPower, 1e-18) / std::max(fundMag * fundMag, 1e-18));
        REQUIRE(aliasDb < -70.0);
        REQUIRE(maxSig > 0.1);
    }

    // 4. Pultec EQP-1A Low-End Trick: +5 dB Low Weight lifts 60 Hz AND dips 210 Hz
    {
        auto measureGainDb = [&](double freqHz) -> double {
            stoneridge::vocal::ConsoleEqStage eq;
            eq.Prepare(kFs);
            std::vector<float> buf(4096);
            for (size_t i = 0; i < buf.size(); ++i) {
                buf[i] = 0.25f * static_cast<float>(std::sin(kTwoPi * freqHz * i / kFs));
            }
            {
                stoneridge::ScopedRealtimeThread rtGuard;
                eq.ProcessBlock(buf.data(), static_cast<uint32_t>(buf.size()),
                                static_cast<uint32_t>(stoneridge::vocal::ConsoleEqModel::PultecEqp1a), 5.0f, 0.0f, 0.0f);
            }
            double sumSq = 0.0;
            for (size_t i = 2048; i < 4096; ++i) sumSq += static_cast<double>(buf[i]) * buf[i];
            const double outRms = std::sqrt(sumSq / 2048.0);
            return 20.0 * std::log10(outRms / (0.25 * 0.70710678));
        };
        const double g60  = measureGainDb(60.0);
        const double g210 = measureGainDb(210.0);
        REQUIRE(g60 > 3.5);
        REQUIRE(g210 < -1.0);
    }
}

// ==============================================================================
// 16. Phase 4: Flagship Mic Locker (V1 Curves, Head-Amp Harmonics & V5 1/r Proximity)
// ==============================================================================
TEST_CASE("Phase 4: MicLocker V1 curve shapes, head-amp harmonics & V5 1/r spherical proximity", "[phase4][dsp]") {
    constexpr double kFs = 48000.0;

    auto measureGainAt = [&](uint32_t model, float distCm, double freqHz) -> double {
        stoneridge::vocal::MicLockerStage mic;
        mic.Prepare(kFs);
        std::vector<float> buf(4096);
        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = 0.05f * static_cast<float>(std::sin(kTwoPi * freqHz * i / kFs));
        }
        {
            stoneridge::ScopedRealtimeThread rtGuard;
            mic.ProcessBlock(buf.data(), static_cast<uint32_t>(buf.size()), model, distCm);
        }
        double sumSq = 0.0;
        for (size_t i = 2048; i < 4096; ++i) sumSq += static_cast<double>(buf[i]) * buf[i];
        return 20.0 * std::log10(std::sqrt(sumSq / 2048.0) / (0.05 * 0.70710678));
    };

    // 1. Test V1 spot checks against target CSV curves (C800G 12.5k air vs U87 flat vs SM7B rolloff)
    REQUIRE(measureGainAt(static_cast<uint32_t>(stoneridge::vocal::MicModel::SonyC800G), 15.0f, 12500.0) == Catch::Approx(4.8).margin(0.6));
    REQUIRE(measureGainAt(static_cast<uint32_t>(stoneridge::vocal::MicModel::NeumannU87Ai), 15.0f, 1000.0) == Catch::Approx(0.0).margin(0.4));
    REQUIRE(measureGainAt(static_cast<uint32_t>(stoneridge::vocal::MicModel::NeumannU87Ai), 15.0f, 10000.0) == Catch::Approx(-0.2).margin(0.5));
    REQUIRE(measureGainAt(static_cast<uint32_t>(stoneridge::vocal::MicModel::NeumannU47), 15.0f, 2000.0) == Catch::Approx(2.0).margin(0.6));
    REQUIRE(measureGainAt(static_cast<uint32_t>(stoneridge::vocal::MicModel::ShureSm7b), 15.0f, 4000.0) == Catch::Approx(3.2).margin(0.6));
    REQUIRE(measureGainAt(static_cast<uint32_t>(stoneridge::vocal::MicModel::ShureSm7b), 15.0f, 12500.0) == Catch::Approx(-4.0).margin(0.6));

    // 2. Test V5 Analytic Cardioid Spherical-Wave Proximity:
    //    H_prox(j*w, r) = (j*w + alpha*c/r_tgt) / (j*w + alpha*c/r_ref)
    auto analyticProxDb = [&](float distCm, double freqHz) -> double {
        constexpr double c = 343.0;
        constexpr double alpha = 0.14;
        const double w = kTwoPi * freqHz;
        const double wz = alpha * c / (static_cast<double>(distCm) * 0.01);
        const double wp = alpha * c / 0.15;
        const double mag = std::sqrt((w * w + wz * wz) / (w * w + wp * wp));
        return 20.0 * std::log10(mag);
    };
    const double measClose100 = measureGainAt(0u, 5.0f, 100.0);
    const double expClose100  = analyticProxDb(5.0f, 100.0);
    const double measFar100   = measureGainAt(0u, 30.0f, 100.0);
    const double expFar100    = analyticProxDb(30.0f, 100.0);
    REQUIRE(measClose100 == Catch::Approx(expClose100).margin(0.5));
    REQUIRE(measFar100 == Catch::Approx(expFar100).margin(0.5));
    REQUIRE(measClose100 > measFar100 + 3.0);
}
