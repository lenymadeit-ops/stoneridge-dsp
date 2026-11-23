#include "dsp/InputProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <immintrin.h>

namespace stoneridge {

namespace {

constexpr float kPi     = 3.14159265358979323846f;
constexpr float kMinLin = 1e-5f; // -100 dBFS

inline float DbToLinear(float db) noexcept {
    return std::pow(10.0f, db * 0.05f);
}

inline float LinearToDb(float lin) noexcept {
    return 20.0f * std::log10(std::max(lin, kMinLin));
}

// Exact continuous C^2 antiderivative F1(x) over the entire soft-clipper curve:
// - |x| <= kKnee:         f(x) = x                            -> F1(x) = 0.5 * x^2
// - kKnee < |x| <= kLim:  f(x) = sgn*(k + t - t^3/(3*S^2))    -> F1(x) = 0.5*k^2 + k*t + 0.5*t^2 - t^4/(12*S^2)
// - |x| > kLim:           f(x) = sgn*(k + (2/3)*S)            -> F1(x) = 0.5*k^2 + k*S + (5/12)*S^2 + Ceil*(t - S)
constexpr float kClipKnee    = 0.841395141645195f;  // -1.5 dBFS
constexpr float kClipCeiling = 0.9660508789898134f; // -0.3 dBFS
constexpr float kClipSpan    = (kClipCeiling - kClipKnee) * 1.5f;
constexpr float kClipSpanSq  = kClipSpan * kClipSpan;
constexpr float kClipInvSpanSq = 1.0f / kClipSpanSq;

inline float SoftClipF1(float x) noexcept {
    const float a = std::fabs(x);
    if (a <= kClipKnee) {
        return 0.5f * a * a;
    }
    const float t = a - kClipKnee;
    if (t <= kClipSpan) {
        const float t2 = t * t;
        return 0.5f * kClipKnee * kClipKnee + kClipKnee * t + 0.5f * t2 - (t2 * t2) * (kClipInvSpanSq / 12.0f);
    }
    return 0.5f * kClipKnee * kClipKnee + kClipKnee * kClipSpan + (5.0f / 12.0f) * kClipSpanSq
         + kClipCeiling * (t - kClipSpan);
}

inline float SoftClipEval(float x) noexcept {
    const float a = std::fabs(x);
    if (a <= kClipKnee) return x;
    const float sign = (x >= 0.0f) ? 1.0f : -1.0f;
    const float t = a - kClipKnee;
    if (t >= kClipSpan) return sign * kClipCeiling;
    return sign * (kClipKnee + t - (t * t * t) * (kClipInvSpanSq / 3.0f));
}

} // namespace

// 1st-Order Polynomial ADAA Soft-Clipper:
// Bit-exact linear below -1.5 dBFS, continuous C^2 ADAA1 saturation up to -0.3 dBFS.
float InputProcessor::AdaaClipper::Process(float x, uint32_t ch) noexcept {
    const float prev = prevU[ch];
    prevU[ch] = x;

    if (std::fabs(x) <= kClipKnee && std::fabs(prev) <= kClipKnee) {
        return x;
    }

    const float diff = x - prev;
    if (std::fabs(diff) > 1e-4f) {
        return (SoftClipF1(x) - SoftClipF1(prev)) / diff;
    }
    return SoftClipEval(0.5f * (x + prev));
}

void InputProcessor::EnableDenormalProtection() noexcept {
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
}

void InputProcessor::Biquad::SetHpf(float cutoffHz, double sampleRate) noexcept {
    if (std::fabs(cutoffHz - lastParam) < 0.25f) return;
    lastParam = cutoffHz;
    if (cutoffHz <= 1.0f) {
        b0 = 1.0f; b1 = 0.0f; b2 = 0.0f; a1 = 0.0f; a2 = 0.0f;
        return;
    }
    const float f = std::clamp(cutoffHz, 15.0f, static_cast<float>(sampleRate * 0.45));
    const float k = std::tan(kPi * f / static_cast<float>(sampleRate));
    constexpr float kSqrt2 = 1.4142135623730951f;
    const float norm = 1.0f / (1.0f + kSqrt2 * k + k * k);
    b0 = norm; b1 = -2.0f * norm; b2 = norm;
    a1 = 2.0f * (k * k - 1.0f) * norm;
    a2 = (1.0f - kSqrt2 * k + k * k) * norm;
}

void InputProcessor::Biquad::SetLowpass(float cutoffHz, double sampleRate) noexcept {
    if (std::fabs(cutoffHz - lastParam) < 0.25f) return;
    lastParam = cutoffHz;
    const float f = std::clamp(cutoffHz, 20.0f, static_cast<float>(sampleRate * 0.45));
    const float k = std::tan(kPi * f / static_cast<float>(sampleRate));
    constexpr float kSqrt2 = 1.4142135623730951f;
    const float norm = 1.0f / (1.0f + kSqrt2 * k + k * k);
    b0 = k * k * norm; b1 = 2.0f * b0; b2 = b0;
    a1 = 2.0f * (k * k - 1.0f) * norm;
    a2 = (1.0f - kSqrt2 * k + k * k) * norm;
}

void InputProcessor::Biquad::SetNotch(float freqHz, float q, double sampleRate) noexcept {
    if (std::fabs(freqHz - lastParam) < 0.25f) return;
    lastParam = freqHz;
    if (freqHz <= 1.0f) {
        b0 = 1.0f; b1 = 0.0f; b2 = 0.0f; a1 = 0.0f; a2 = 0.0f;
        return;
    }
    const float w0 = 2.0f * kPi * std::clamp(freqHz, 15.0f, static_cast<float>(sampleRate * 0.45)) / static_cast<float>(sampleRate);
    const float cs = std::cos(w0);
    const float alpha = std::sin(w0) / (2.0f * q);
    const float invA0 = 1.0f / (1.0f + alpha);
    b0 = 1.0f * invA0;
    b1 = -2.0f * cs * invA0;
    b2 = 1.0f * invA0;
    a1 = -2.0f * cs * invA0;
    a2 = (1.0f - alpha) * invA0;
}

void InputProcessor::Biquad::SetBandpass(float freqHz, float q, double sampleRate) noexcept {
    if (std::fabs(freqHz - lastParam) < 0.25f) return;
    lastParam = freqHz;
    const float w0 = 2.0f * kPi * std::clamp(freqHz, 20.0f, static_cast<float>(sampleRate * 0.45)) / static_cast<float>(sampleRate);
    const float sn = std::sin(w0);
    const float cs = std::cos(w0);
    const float alpha = sn / (2.0f * q);
    const float invA0 = 1.0f / (1.0f + alpha);
    b0 = alpha * invA0;
    b1 = 0.0f;
    b2 = -alpha * invA0;
    a1 = -2.0f * cs * invA0;
    a2 = (1.0f - alpha) * invA0;
}

void InputProcessor::Biquad::SetPeaking(float freqHz, float q, float gainDb, double sampleRate) noexcept {
    if (std::fabs(freqHz - lastParam) < 0.25f) return;
    lastParam = freqHz;
    const float A  = std::pow(10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * kPi * std::clamp(freqHz, 20.0f, static_cast<float>(sampleRate * 0.45)) / static_cast<float>(sampleRate);
    const float cs = std::cos(w0);
    const float alpha = std::sin(w0) / (2.0f * q);
    const float invA0 = 1.0f / (1.0f + alpha / A);
    b0 = (1.0f + alpha * A) * invA0;
    b1 = (-2.0f * cs) * invA0;
    b2 = (1.0f - alpha * A) * invA0;
    a1 = (-2.0f * cs) * invA0;
    a2 = (1.0f - alpha / A) * invA0;
}

void InputProcessor::Biquad::SetLowShelf(float freqHz, float q, float gainDb, double sampleRate) noexcept {
    if (std::fabs(gainDb - lastParam) < 0.01f) return;
    lastParam = gainDb;
    const float A  = std::pow(10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * kPi * std::clamp(freqHz, 20.0f, static_cast<float>(sampleRate * 0.45)) / static_cast<float>(sampleRate);
    const float cs = std::cos(w0);
    const float sn = std::sin(w0);
    const float alpha = sn / (2.0f * std::max(q, 0.25f));
    const float twoSqrtAAlpha = 2.0f * std::sqrt(A) * alpha;
    const float invA0 = 1.0f / ((A + 1.0f) + (A - 1.0f) * cs + twoSqrtAAlpha);
    b0 = (A * ((A + 1.0f) - (A - 1.0f) * cs + twoSqrtAAlpha)) * invA0;
    b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs)) * invA0;
    b2 = (A * ((A + 1.0f) - (A - 1.0f) * cs - twoSqrtAAlpha)) * invA0;
    a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cs)) * invA0;
    a2 = ((A + 1.0f) + (A - 1.0f) * cs - twoSqrtAAlpha) * invA0;
}

void InputProcessor::Biquad::SetHighShelf(float freqHz, float q, float gainDb, double sampleRate) noexcept {
    if (std::fabs(freqHz - lastParam) < 0.25f) return;
    lastParam = freqHz;
    const float A  = std::pow(10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * kPi * std::clamp(freqHz, 20.0f, static_cast<float>(sampleRate * 0.45)) / static_cast<float>(sampleRate);
    const float cs = std::cos(w0);
    const float sn = std::sin(w0);
    const float alpha = sn / (2.0f * q);
    const float twoSqrtAAlpha = 2.0f * std::sqrt(A) * alpha;
    const float invA0 = 1.0f / ((A + 1.0f) - (A - 1.0f) * cs + twoSqrtAAlpha);
    b0 = (A * ((A + 1.0f) + (A - 1.0f) * cs + twoSqrtAAlpha)) * invA0;
    b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs)) * invA0;
    b2 = (A * ((A + 1.0f) + (A - 1.0f) * cs - twoSqrtAAlpha)) * invA0;
    a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cs)) * invA0;
    a2 = ((A + 1.0f) - (A - 1.0f) * cs - twoSqrtAAlpha) * invA0;
}

void InputProcessor::Prepare(double sampleRate, uint32_t /*maxChannels*/) noexcept {
    m_sampleRate = (sampleRate > 0.0) ? sampleRate : 48000.0;
    m_gainCoeff  = 1.0f - std::exp(-1.0f / (0.005f * static_cast<float>(m_sampleRate)));
    m_dcCoeff    = 1.0f - (2.0f * kPi * 5.0f / static_cast<float>(m_sampleRate));

    m_lookDelaySamples = std::clamp<uint32_t>(static_cast<uint32_t>(m_sampleRate * 0.001), 16u, kLookAheadMax - 1u);

    m_envAtkFast = 1.0f - std::exp(-1.0f / (0.002f * static_cast<float>(m_sampleRate)));
    m_envRelFast = 1.0f - std::exp(-1.0f / (0.040f * static_cast<float>(m_sampleRate)));
    m_envRelSlow = 1.0f - std::exp(-1.0f / (0.180f * static_cast<float>(m_sampleRate)));

    // Precompute Goertzel coefficients for 50 Hz & 60 Hz mains hum detection
    m_g50Coeff = 2.0 * std::cos(2.0 * kPi * 50.0 / m_sampleRate);
    m_g60Coeff = 2.0 * std::cos(2.0 * kPi * 60.0 / m_sampleRate);

    // Invalidate caches
    m_cachedGainDb   = -999.0f;
    m_cachedThreshDb = -999.0f;
    m_cachedHystDb   = -999.0f;
    m_cachedRangeDb  = -999.0f;
    m_cachedAtkMs    = -999.0f;
    m_cachedHoldMs   = -999.0f;
    m_cachedRelMs    = -999.0f;

    m_audioHpf.lastParam     = -1.0f;
    m_sidechainHpf.lastParam = -1.0f;
    m_humNotch1.lastParam    = -1.0f;
    m_humNotch2.lastParam    = -1.0f;
    m_eqDeMud.lastParam      = -1.0f;
    m_eqDeHarsh.lastParam    = -1.0f;
    m_eqAir.lastParam        = -1.0f;
    m_deEssBp.lastParam      = -1.0f;
    m_plosiveLp.lastParam    = -1.0f;
    m_ultraLpf1.lastParam    = -1.0f;
    m_ultraLpf2.lastParam    = -1.0f;

    m_audioHpf.SetHpf(80.0f, m_sampleRate);
    m_eqDeMud.SetPeaking(200.0f, 1.2f, -2.0f, m_sampleRate);
    m_eqDeHarsh.SetPeaking(7500.0f, 1.8f, -1.8f, m_sampleRate);
    m_eqAir.SetHighShelf(11500.0f, 0.707f, 2.0f, m_sampleRate);
    m_deEssBp.SetBandpass(6500.0f, 1.8f, m_sampleRate);
    m_plosiveLp.SetLowpass(140.0f, m_sampleRate);
    if (m_sampleRate >= 80000.0) {
        m_ultraLpf1.SetLowpass(22000.0f, m_sampleRate);
        m_ultraLpf2.SetLowpass(22000.0f, m_sampleRate);
    }

    m_cachedCalibStrength = -1.0f;
    for (auto& b : m_calibBiquads) {
        b.lastParam = -999.0f;
    }
    m_loudnessMatcher.Prepare(m_sampleRate);
    m_micLocker.Prepare(m_sampleRate);
    m_preampStage.Prepare(m_sampleRate);
    m_fet1176.Prepare(m_sampleRate);
    m_optoLa2a.Prepare(m_sampleRate);
    m_consoleEq.Prepare(m_sampleRate);

    Reset();
}

void InputProcessor::Reset() noexcept {
    m_gainSmoothed  = 1.0f;
    m_gateEnv       = 1.0f;
    m_holdCounter   = 0;
    m_gateOpen      = true;
    m_dcPrevX[0]    = m_dcPrevX[1] = 0.0f;
    m_dcPrevY[0]    = m_dcPrevY[1] = 0.0f;
    m_lookL.fill(0.0f);
    m_lookR.fill(0.0f);
    m_lookWriteIdx  = 0;
    m_deEssEnv      = 0.0f;
    m_deEssGain     = 1.0f;
    m_plosiveLowEnv = 0.0f;
    m_plosiveMidEnv = 0.0f;
    m_plosiveGain   = 1.0f;
    m_levelerRmsSq  = 0.0f;
    m_levelerGain   = 1.0f;

    m_audioHpf.Reset();
    m_sidechainHpf.Reset();
    m_humNotch1.Reset();
    m_humNotch2.Reset();
    m_eqDeMud.Reset();
    m_eqDeHarsh.Reset();
    m_eqAir.Reset();
    m_deEssBp.Reset();
    m_plosiveLp.Reset();
    m_ultraLpf1.Reset();
    m_ultraLpf2.Reset();
    for (auto& b : m_calibBiquads) b.Reset();
    m_clipper.Reset();
    m_loudnessMatcher.Reset();
    m_micLocker.Reset();
    m_preampStage.Reset();
    m_fet1176.Reset();
    m_optoLa2a.Reset();
    m_consoleEq.Reset();
}

void InputProcessor::StartLearnRoomNoise() noexcept {
    m_learnTargetSamples = static_cast<uint32_t>(m_sampleRate * 2.0); // 2 seconds
    m_learnCount  = 0;
    m_learnSumSq  = 0.0;
    m_learnPeakLin = 0.0f;
    m_g50Q1 = m_g50Q2 = 0.0;
    m_g60Q1 = m_g60Q2 = 0.0;
    m_learnResult = {};
    m_learnActive = true;
}

void InputProcessor::AccumulateLearnSample(float sample) noexcept {
    if (!m_learnActive) return;

    m_learnSumSq += static_cast<double>(sample) * static_cast<double>(sample);
    m_learnPeakLin = std::max(m_learnPeakLin, std::fabs(sample));

    // Goertzel resonator for 50 Hz and 60 Hz mains hum
    const double q50 = static_cast<double>(sample) + m_g50Coeff * m_g50Q1 - m_g50Q2;
    m_g50Q2 = m_g50Q1;
    m_g50Q1 = q50;

    const double q60 = static_cast<double>(sample) + m_g60Coeff * m_g60Q1 - m_g60Q2;
    m_g60Q2 = m_g60Q1;
    m_g60Q1 = q60;

    ++m_learnCount;
    m_learnResult.progress = static_cast<float>(m_learnCount) / static_cast<float>(std::max(1u, m_learnTargetSamples));

    if (m_learnCount >= m_learnTargetSamples) {
        m_learnActive = false;
        const double n = static_cast<double>(m_learnCount);
        const float rmsLin = static_cast<float>(std::sqrt(m_learnSumSq / n));
        const float floorDb = LinearToDb(rmsLin);
        const float peakDb  = LinearToDb(m_learnPeakLin);

        // Compute 50 Hz & 60 Hz Goertzel power normalized by total energy
        const double p50 = (m_g50Q1 * m_g50Q1 + m_g50Q2 * m_g50Q2 - m_g50Coeff * m_g50Q1 * m_g50Q2) / (n * n * 0.25);
        const double p60 = (m_g60Q1 * m_g60Q1 + m_g60Q2 * m_g60Q2 - m_g60Coeff * m_g60Q1 * m_g60Q2) / (n * n * 0.25);
        const double totalP = std::max(m_learnSumSq / n, 1e-12);

        float humHz = 0.0f;
        if (p60 > totalP * 0.25 && p60 >= p50) humHz = 60.0f;
        else if (p50 > totalP * 0.25) humHz = 50.0f;

        m_learnResult.completed           = true;
        m_learnResult.progress            = 1.0f;
        m_learnResult.noiseFloorDb        = floorDb;
        m_learnResult.recommendedThreshDb = std::clamp(std::max(floorDb + 8.0f, peakDb + 3.0f), -65.0f, -20.0f);
        m_learnResult.recommendedHumHz    = humHz;
    }
}

void InputProcessor::StartPreampAutoLevel(float currentHardwarePgaDb, bool hasHardwarePga) noexcept {
    m_preampTargetSamples = static_cast<uint32_t>(m_sampleRate * 3.0); // 3 seconds vocal window
    m_preampCount         = 0;
    m_preampStartPgaDb    = currentHardwarePgaDb;
    m_hasHwPga            = hasHardwarePga;
    m_preampPeakLin       = 0.0f;
    m_preampResult        = {};
    m_preampCalibActive   = true;
}

void InputProcessor::AccumulatePreampSample(float sample) noexcept {
    if (!m_preampCalibActive) return;

    m_preampPeakLin = std::max(m_preampPeakLin, std::fabs(sample));
    ++m_preampCount;
    m_preampResult.progress = static_cast<float>(m_preampCount) / static_cast<float>(std::max(1u, m_preampTargetSamples));

    if (m_preampCount >= m_preampTargetSamples) {
        m_preampCalibActive = false;
        const float peakDb  = LinearToDb(std::max(m_preampPeakLin, kMinLin));
        // Sweet spot target: -9.0 dBFS peak gives 9 dB transient headroom before ADC clipping
        const float deltaDb = -9.0f - peakDb;
        const float wantedPga = m_preampStartPgaDb + deltaDb;
        // Snap hardware PGA to [-63.5, 0.0] dB in exact 0.5 dB steps
        const float hwPga = m_hasHwPga
            ? std::clamp(std::round(wantedPga * 2.0f) * 0.5f, -63.5f, 0.0f)
            : m_preampStartPgaDb;
        const float residual = wantedPga - hwPga;
        const float leftover = (peakDb > -65.0f)
            ? std::clamp(std::round(residual * 2.0f) * 0.5f, -24.0f, 12.0f)
            : 0.0f;

        m_preampResult.completed         = true;
        m_preampResult.progress          = 1.0f;
        m_preampResult.measuredPeakDb    = peakDb;
        m_preampResult.recommendedPgaDb  = hwPga;
        m_preampResult.recommendedTrimDb = leftover;
    }
}

void InputProcessor::UpdateCachedParams(const InputDspParams& params) noexcept {
    if (params.gainDb != m_cachedGainDb || params.mute != m_cachedMute) {
        m_cachedGainDb  = params.gainDb;
        m_cachedMute    = params.mute;
        m_targetGainLin = params.mute ? 0.0f : DbToLinear(std::clamp(params.gainDb, -24.0f, 24.0f));
    }

    if (params.humNotchFreqHz != m_humNotch1.lastParam) {
        m_humNotch1.SetNotch(params.humNotchFreqHz, 8.0f, m_sampleRate);
        m_humNotch2.SetNotch(params.humNotchFreqHz * 2.0f, 8.0f, m_sampleRate);
    }

    if (params.capsuleCorrectionEnabled && kEm91cuCalibrationMeasured) {
        const float str = std::clamp(params.capsuleCorrectionStrength, 0.0f, 1.0f);
        if (std::fabs(str - m_cachedCalibStrength) >= 0.005f) {
            m_cachedCalibStrength = str;
            for (uint32_t k = 0; k < kEm91cuCalibSections; ++k) {
                const auto& sec = kEm91cuCalibCurve[k];
                const float g = sec.gainDb * str;
                m_calibBiquads[k].lastParam = -999.0f;
                if (sec.kind == 1u) {
                    m_calibBiquads[k].SetLowShelf(sec.freqHz, sec.q, g, m_sampleRate);
                } else if (sec.kind == 2u) {
                    m_calibBiquads[k].SetHighShelf(sec.freqHz, sec.q, g, m_sampleRate);
                } else {
                    m_calibBiquads[k].SetPeaking(sec.freqHz, sec.q, g, m_sampleRate);
                }
            }
        }
    }

    if (params.gateEnabled) {
        m_sidechainHpf.SetHpf(params.sidechainHpfHz, m_sampleRate);

        if (params.gateThresholdDb != m_cachedThreshDb || params.gateHysteresisDb != m_cachedHystDb) {
            m_cachedThreshDb = params.gateThresholdDb;
            m_cachedHystDb   = params.gateHysteresisDb;
            const float openDb  = std::clamp(params.gateThresholdDb, -80.0f, 0.0f);
            const float hystDb  = std::clamp(params.gateHysteresisDb, 0.0f, 12.0f);
            const float closeDb = std::max(-96.0f, openDb - hystDb);
            m_openThreshLin  = DbToLinear(openDb);
            m_closeThreshLin = DbToLinear(closeDb);
        }

        if (params.gateRangeDb != m_cachedRangeDb) {
            m_cachedRangeDb = params.gateRangeDb;
            m_floorLin      = DbToLinear(std::clamp(params.gateRangeDb, -80.0f, 0.0f));
        }

        if (params.gateAttackMs != m_cachedAtkMs) {
            m_cachedAtkMs = params.gateAttackMs;
            const float atkSec = std::max(0.0001f, params.gateAttackMs * 0.001f);
            m_attackCoeff = 1.0f - std::exp(-1.0f / (atkSec * static_cast<float>(m_sampleRate)));
        }

        if (params.gateReleaseMs != m_cachedRelMs) {
            m_cachedRelMs = params.gateReleaseMs;
            const float relSec = std::max(0.001f, params.gateReleaseMs * 0.001f);
            m_releaseCoeff = 1.0f - std::exp(-1.0f / (relSec * static_cast<float>(m_sampleRate)));
        }

        if (params.gateHoldMs != m_cachedHoldMs) {
            m_cachedHoldMs = params.gateHoldMs;
            m_holdSamples  = static_cast<uint32_t>(std::max(0.0f, params.gateHoldMs * 0.001f) * m_sampleRate);
        }
    }
}

void InputProcessor::ProcessMono(float* mono, uint32_t numFrames,
                                 const InputDspParams& params, InputDspTelemetry& outTelemetry) noexcept {
    ProcessStereo(mono, nullptr, numFrames, params, outTelemetry);
}

void InputProcessor::ProcessStereo(float* left, float* right, uint32_t numFrames,
                                   const InputDspParams& params, InputDspTelemetry& outTelemetry) noexcept {
    if (!left || numFrames == 0) {
        return;
    }

    UpdateCachedParams(params);

    const bool gateOn    = params.gateEnabled;
    const bool lookOn    = gateOn && params.gateLookAheadEnabled;
    const bool hpfOn     = params.audioHpfEnabled;
    const bool ultraOn   = params.ultrasonicLpfEnabled && (m_sampleRate >= 80000.0);
    const bool humOn     = params.humNotchFreqHz > 1.0f;
    const bool eqOn      = params.capsuleEqEnabled;
    const bool calibOn   = params.capsuleCorrectionEnabled && kEm91cuCalibrationMeasured && (params.capsuleCorrectionStrength > 0.01f);
    const bool plosiveOn = params.plosiveGuardEnabled;
    const bool deEssOn   = params.deEsserAmount > 0.01f;
    const bool fetOn     = params.fetPeakCatchAmount > 0.01f;
    const bool levelerOn = params.vocalLevelerAmount > 0.01f;
    const bool clipOn    = params.softClipEnabled;
    const uint32_t routeMode = (params.mic2RoutingMode != 0u) ? params.mic2RoutingMode : (params.dualTrackSafetyMode ? 2u : 0u);
    const bool dualSafe  = (routeMode == 2u) && (right != nullptr);
    const bool preDynStem = (routeMode == 1u) && (right != nullptr);
    const bool abBypass  = params.abLevelMatchedBypass;
    const bool monoFast  = (right == nullptr);
    const bool runCh1    = !monoFast && !dualSafe && !preDynStem && (right != nullptr);

    const float tgtGain  = m_targetGainLin;
    const float gCoeff   = m_gainCoeff;

    if (!gateOn) {
        m_gateOpen    = true;
        m_gateEnv     = 1.0f;
        m_holdCounter = 0;
    }

    float peakLin      = 0.0f;
    float sumSq        = 0.0f;
    float minGateGain  = 1.0f;
    float minDeEssGain = 1.0f;
    float minPlosGain  = 1.0f;
    float minFetGrDb   = 0.0f;
    float minLevGrDb   = 0.0f;
    bool  adcClip      = false;

    uint32_t offset = 0;
    while (offset < numFrames) {
        const uint32_t chunk = std::min<uint32_t>(numFrames - offset, 2048u);
        float* cL = left + offset;
        float* cR = right ? (right + offset) : nullptr;

        for (uint32_t i = 0; i < chunk; ++i) {
            m_gainSmoothed += (tgtGain - m_gainSmoothed) * gCoeff;

            // 0. Pre-DSP Hardware ADC Clip Detection + Always-On 5 Hz DC Blocker
            const float rawL = cL[i];
            const float rawR = runCh1 ? cR[i] : rawL;
            if (std::fabs(rawL) >= 0.9988f || (runCh1 && std::fabs(rawR) >= 0.9988f)) {
                adcClip = true;
            }

            float l = rawL - m_dcPrevX[0] + m_dcCoeff * m_dcPrevY[0];
            m_dcPrevX[0] = rawL;
            m_dcPrevY[0] = l;

            float r = l;
            if (!monoFast && cR) {
                r = cR[i] - m_dcPrevX[1] + m_dcCoeff * m_dcPrevY[1];
                m_dcPrevX[1] = cR[i];
                m_dcPrevY[1] = r;
            }
            const float rawBackupL = l;
            const float rawBackupR = r;
            m_rawScratch[i] = rawBackupL;

            // 1. Optional Room-Noise / Preamp Auto-Level Calibration Samplers
            if (m_learnActive) {
                AccumulateLearnSample(l);
            }
            if (m_preampCalibActive) {
                AccumulatePreampSample(l);
            }

            // 1.5 Automatic 22 kHz 4th-order Ultrasonic ADC Modulator Filter at >=88.2 kHz
            if (ultraOn) {
                l = m_ultraLpf2.Process(m_ultraLpf1.Process(l, 0), 0);
                if (runCh1) r = m_ultraLpf2.Process(m_ultraLpf1.Process(r, 1), 1);
            }

            // 2. Mains Hum Notch (50/60 Hz + 2nd harmonic) & 80 Hz Rumble HPF
            if (humOn) {
                l = m_humNotch2.Process(m_humNotch1.Process(l, 0), 0);
                if (runCh1) r = m_humNotch2.Process(m_humNotch1.Process(r, 1), 1);
            }
            if (hpfOn) {
                l = m_audioHpf.Process(l, 0);
                if (runCh1) r = m_audioHpf.Process(r, 1);
            }

            // 3. Dynamic Plosive / Pop Suppressor (< 140 Hz)
            if (plosiveOn) {
                const float lowL = m_plosiveLp.Process(l, 0);
                const float midL = l - lowL;
                const float aLow = std::fabs(lowL);
                const float aMid = std::fabs(midL);
                m_plosiveLowEnv += (aLow - m_plosiveLowEnv) * (aLow > m_plosiveLowEnv ? m_envAtkFast : m_envRelFast);
                m_plosiveMidEnv += (aMid - m_plosiveMidEnv) * (aMid > m_plosiveMidEnv ? m_envAtkFast : m_envRelFast);

                float targetPlosGain = 1.0f;
                if (m_plosiveLowEnv > 0.04f && m_plosiveLowEnv > m_plosiveMidEnv * 2.2f) {
                    const float ratio = (m_plosiveMidEnv * 2.2f) / m_plosiveLowEnv;
                    targetPlosGain = std::clamp(ratio, 0.2f, 1.0f);
                }
                m_plosiveGain += (targetPlosGain - m_plosiveGain) * (targetPlosGain < m_plosiveGain ? m_envAtkFast : m_envRelFast);
                l = midL + lowL * m_plosiveGain;
                if (runCh1) {
                    const float lowR = m_plosiveLp.Process(r, 1);
                    r = (r - lowR) + lowR * m_plosiveGain;
                }
                minPlosGain = std::min(minPlosGain, m_plosiveGain);
            }

            // 4. Measured Pixel 11 Pro / Isolation Shield Correction + Optional Voice Contour EQ
            if (calibOn) {
                for (uint32_t k = 0; k < kEm91cuCalibSections; ++k) {
                    l = m_calibBiquads[k].Process(l, 0);
                    if (runCh1) r = m_calibBiquads[k].Process(r, 1);
                }
            }
            if (eqOn) {
                l = m_eqAir.Process(m_eqDeHarsh.Process(m_eqDeMud.Process(l, 0), 0), 0);
                if (runCh1) {
                    r = m_eqAir.Process(m_eqDeHarsh.Process(m_eqDeMud.Process(r, 1), 1), 1);
                }
            }

            // 5. Zipper-free Input Gain
            l *= m_gainSmoothed;
            r = runCh1 ? (r * m_gainSmoothed) : l;
            if (cR && dualSafe) {
                cR[i] = rawBackupR;
            }
            cL[i] = l;
            if (runCh1 && cR) cR[i] = r;
        }

        // 5.2 Flagship Microphone Locker + 1/r Spherical Proximity -> 5.5 Ferromagnetic Preamp Stage
        m_micLocker.ProcessBlock(cL, chunk, params.micModel, params.virtualDistanceCm);
        m_preampStage.ProcessBlock(cL, chunk, params.preampModel, params.preampDriveDb, params.transformerIronAmount);
        if (cR && preDynStem) {
            std::memcpy(cR, cL, chunk * sizeof(float));
        }

        // 6. Broadcast Noise Gate & 7. Zero-Latency Dynamic De-Esser
        if (gateOn || deEssOn) {
            for (uint32_t i = 0; i < chunk; ++i) {
                float l = cL[i];
                float r = (runCh1 && cR) ? cR[i] : l;

                if (gateOn) {
                    const float scL = std::fabs(m_sidechainHpf.Process(l, 0));
                    const float scR = runCh1 ? std::fabs(m_sidechainHpf.Process(r, 1)) : scL;
                    const float detector = std::max(scL, scR);

                    if (lookOn) {
                        m_lookL[m_lookWriteIdx] = l;
                        m_lookR[m_lookWriteIdx] = r;
                        const uint32_t readIdx = (m_lookWriteIdx + kLookAheadMax - m_lookDelaySamples) & (kLookAheadMax - 1u);
                        l = m_lookL[readIdx];
                        if (runCh1) r = m_lookR[readIdx];
                        m_lookWriteIdx = (m_lookWriteIdx + 1u) & (kLookAheadMax - 1u);
                    }

                    if (m_gateOpen) {
                        if (detector >= m_closeThreshLin) {
                            m_holdCounter = m_holdSamples;
                        } else if (m_holdCounter > 0) {
                            --m_holdCounter;
                        } else {
                            m_gateOpen = false;
                        }
                    } else if (detector >= m_openThreshLin) {
                        m_gateOpen    = true;
                        m_holdCounter = m_holdSamples;
                    }

                    const float targetGate = m_gateOpen ? 1.0f : m_floorLin;
                    const float coeff = (targetGate > m_gateEnv) ? m_attackCoeff : m_releaseCoeff;
                    m_gateEnv += (targetGate - m_gateEnv) * coeff;

                    l *= m_gateEnv;
                    if (runCh1) r *= m_gateEnv;
                    minGateGain = std::min(minGateGain, m_gateEnv);
                }

                if (deEssOn) {
                    const float sib = std::fabs(m_deEssBp.Process(l, 0));
                    m_deEssEnv += (sib - m_deEssEnv) * (sib > m_deEssEnv ? m_envAtkFast : m_envRelFast);
                    const float thresh = 0.08f - 0.055f * std::clamp(params.deEsserAmount, 0.0f, 1.0f);
                    float targetDeEss = 1.0f;
                    if (m_deEssEnv > thresh) {
                        targetDeEss = std::clamp(thresh / m_deEssEnv, 0.316f, 1.0f);
                    }
                    m_deEssGain += (targetDeEss - m_deEssGain) * (targetDeEss < m_deEssGain ? m_envAtkFast : m_envRelFast);
                    l *= m_deEssGain;
                    if (runCh1) r *= m_deEssGain;
                    minDeEssGain = std::min(minDeEssGain, m_deEssGain);
                }

                cL[i] = l;
                if (runCh1 && cR) cR[i] = r;
            }
        }

        // 8A. Serial 1176-Style Fast Feedback FET Peak Catcher (40-800 us attack)
        if (fetOn) {
            minFetGrDb = std::min(minFetGrDb, m_fet1176.ProcessBlock(cL, chunk, params.fetPeakCatchAmount, params.fetRatioMode));
        } else {
            m_fet1176.ProcessBlock(cL, chunk, 0.0f, params.fetRatioMode);
        }

        // 8B. Serial LA-2A T4B Dual-Memory Optical Leveler
        if (levelerOn) {
            minLevGrDb = std::min(minLevGrDb, m_optoLa2a.ProcessBlock(cL, chunk, params.vocalLevelerAmount));
        } else {
            m_optoLa2a.ProcessBlock(cL, chunk, 0.0f);
        }

        // 8C. Circuit-Derived Console EQ (Pultec EQP-1A / Neve 1073 / SSL 4000G / API 550A)
        m_consoleEq.ProcessBlock(cL, chunk, params.consoleEqModel, params.consoleLowDb, params.consoleMidDb, params.consoleAirDb);

        // 9. ADAA1 Soft-Clipper + 10. K-Weighted A/B Loudness Matcher + Metering
        for (uint32_t i = 0; i < chunk; ++i) {
            float l = cL[i];
            float r = (runCh1 && cR) ? cR[i] : l;
            if (clipOn) {
                l = m_clipper.Process(l, 0);
                if (runCh1) r = m_clipper.Process(r, 1);
            }

            const float rawB = m_rawScratch[i];
            m_loudnessMatcher.SetGateOpen(m_gateOpen);
            m_loudnessMatcher.SetFreeze(params.freezeAbxGain);
            const float abGain = m_loudnessMatcher.ProcessSample(rawB, l);
            const bool replaceRecord = abBypass && !params.abAuditionMonitorOnly;
            const float outL = replaceRecord ? std::clamp(rawB * abGain, -1.0f, 1.0f) : l;

            cL[i] = outL;
            if (cR && !dualSafe && !preDynStem) {
                cR[i] = runCh1 ? (replaceRecord ? std::clamp(rawB * abGain, -1.0f, 1.0f) : r) : outL;
            }

            const float absL = std::fabs(l);
            const float absR = runCh1 ? std::fabs(r) : absL;
            peakLin = std::max(peakLin, std::max(absL, absR));
            sumSq += l * l + (runCh1 ? (r * r) : 0.0f);
        }

        offset += chunk;
    }

    const float channelDiv = runCh1 ? 2.0f : 1.0f;
    const float rmsLin = std::sqrt(sumSq / (static_cast<float>(numFrames) * channelDiv));

    outTelemetry.peakDb             = LinearToDb(peakLin);
    outTelemetry.rmsDb              = LinearToDb(rmsLin);
    outTelemetry.gateReductionDb    = gateOn ? LinearToDb(minGateGain) : 0.0f;
    outTelemetry.gateIsOpen         = m_gateOpen;
    outTelemetry.deEsserReductionDb = deEssOn ? LinearToDb(minDeEssGain) : 0.0f;
    outTelemetry.plosiveReductionDb = plosiveOn ? LinearToDb(minPlosGain) : 0.0f;
    outTelemetry.fetReductionDb     = fetOn ? minFetGrDb : 0.0f;
    outTelemetry.levelerReductionDb = levelerOn ? minLevGrDb : 0.0f;
    outTelemetry.abMatchGainDb      = m_loudnessMatcher.GetMatchGainDb();
    outTelemetry.adcClipDetected    = adcClip;
}

} // namespace stoneridge
