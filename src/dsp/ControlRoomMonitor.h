#pragma once

// Monitor-only Control-Room Headphone Matrix v2 (runs strictly on the DAC render path;
// NEVER modifies FL Studio recorded ASIO input buffers). Zero RT heap allocations or locks.
//
// Features:
//  1. Comfort Chamber v2: 8x8 AVX2 Householder FDN (7.9 .. 43.1 ms @ up to 192 kHz), dual-band
//     RT60 damping, 0.35 Hz +/- 0.25 ms fractional delay LFO modulation, and velvet-noise ER (0-15 ms).
//  2. Anti-Bleed Dynamic Cue Carve (-1.8 dB @ 2.2 kHz on DAW backing playback when singing).
//  3. Spagnol Spherical-Head + Pinna-Notch Externalizer (280 us Woodworth ITD, 700 Hz head-shadow,
//     7.8 kHz pinna notch, 11.5 kHz elevation cue) + 15 ms smoothed ramps + cached headroom trim.

#include "vocal/SimdKernels.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace stoneridge {

struct ControlRoomParams {
    float directMonitorMix    = 0.0f;   // 0.0 .. 1.0
    float monitorReverbAmount = 0.0f;   // 0.0 .. 1.0
    bool  crossfeedEnabled    = false;  // Spagnol externalizer crossfeed
    bool  cueCarveEnabled     = false;  // -1.8 dB @ 2.2 kHz dynamic backing duck
    float backingHeadroomDb   = 0.0f;   // applied to backing bus when monitor > 0.05
};

class ControlRoomMonitor {
public:
    void Prepare(double sampleRate) noexcept {
        m_fs = (sampleRate > 1000.0) ? sampleRate : 48000.0;
        const float fs = static_cast<float>(m_fs);

        // Allocate FDN lines once in Prepare() so kFdnLineMax = 16384 avoids stack bloat
        m_fdnBuf.assign(static_cast<size_t>(kFdnLineMax) * 8u, 0.0f);
        m_erBuf.assign(kErMax, 0.0f);

        // Prime FDN tap lengths scaled to sample rate (unclamped up to 192 kHz -> 8275 < 16384)
        constexpr float kMs[8] = { 7.9f, 11.3f, 16.7f, 23.1f, 29.4f, 31.7f, 37.2f, 43.1f };
        for (int k = 0; k < 8; ++k) {
            m_fdnDelayBase[k] = std::clamp<float>(kMs[k] * 0.001f * fs, 64.0f, static_cast<float>(kFdnLineMax - 256u));
        }
        m_lfoModSamples = 0.00025f * fs; // +/- 0.25 ms depth
        m_lfoPhaseInc   = 2.0f * kPi * 0.35f / fs;

        // 12-tap velvet-noise early reflections in 1.2 .. 14.8 ms
        constexpr float kErMs[12] = { 1.2f, 2.1f, 3.3f, 4.4f, 5.8f, 6.9f, 8.2f, 9.7f, 10.9f, 12.1f, 13.4f, 14.7f };
        for (int t = 0; t < 12; ++t) {
            m_erDelays[t] = std::clamp<uint32_t>(static_cast<uint32_t>(kErMs[t] * 0.001f * fs), 1u, kErMax - 1u);
            m_erSigns[t] = ((t & 1) == 0) ? 1.0f : -1.0f;
        }

        // Dual-band RT60 damping: 4.8 kHz HF damping + gentle 80 Hz LF sub trim inside feedback
        m_fdnHfDampCoeff = 1.0f - std::exp(-2.0f * kPi * 4800.0f / fs);
        m_fdnLfDampCoeff = 1.0f - std::exp(-2.0f * kPi * 80.0f / fs);

        // Spagnol Externalizer: 280 us Woodworth ITD, 700 Hz head-shadow, 7.8 kHz pinna notch, 11.5 kHz elevation
        m_xfeedDelayLen = std::clamp<uint32_t>(static_cast<uint32_t>(0.000280f * fs), 2u, kXfeedMax - 1u);
        m_xfeedLpCoeff  = 1.0f - std::exp(-2.0f * kPi * 700.0f / fs);
        m_pinnaNotch[0].SetPeaking(7800.0f, 3.2f, -3.2f, fs);
        m_pinnaNotch[1] = m_pinnaNotch[0];
        m_elevCue[0].SetPeaking(11500.0f, 1.8f, 1.5f, fs);
        m_elevCue[1] = m_elevCue[0];

        // 15 ms smooth parameter ramps
        m_rampCoeff = 1.0f - std::exp(-1.0f / (0.015f * fs));
        m_cachedHeadroomDb = -999.0f;
        m_cachedHeadroomLin = 1.0f;

        // Anti-bleed cue carve sidechain envelope & 2.2 kHz bell (Q=1.0)
        m_cueAtkCoeff = 1.0f - std::exp(-1.0f / (0.008f * fs));
        m_cueRelCoeff = 1.0f - std::exp(-1.0f / (0.140f * fs));
        m_cueBell[0].SetPeaking(2200.0f, 1.0f, -1.8f, fs);
        m_cueBell[1] = m_cueBell[0];
        Reset();
    }

    void Reset() noexcept {
        std::fill(m_fdnBuf.begin(), m_fdnBuf.end(), 0.0f);
        std::fill(m_erBuf.begin(), m_erBuf.end(), 0.0f);
        m_fdnWriteIdx.fill(0u);
        m_erWriteIdx = 0u;
        m_fdnHfState.fill(0.0f);
        m_fdnLfState.fill(0.0f);
        m_lfoPhase = 0.0f;
        m_xfeedBufL.fill(0.0f);
        m_xfeedBufR.fill(0.0f);
        m_xfeedWriteIdx = 0u;
        m_xfeedLpL = 0.0f;
        m_xfeedLpR = 0.0f;
        m_pinnaNotch[0].Reset(); m_pinnaNotch[1].Reset();
        m_elevCue[0].Reset();    m_elevCue[1].Reset();
        m_cueEnv = 0.0f;
        m_cueDepthSmooth = 0.0f;
        m_cueBell[0].Reset();    m_cueBell[1].Reset();
        m_hasRamped = false;
        m_monMixSmooth = 0.0f;
        m_verbWetSmooth = 0.0f;
        m_xfeedSmooth = 0.0f;
    }

    void ProcessBlock(const float* dawL, const float* dawR,
                      const float* micL, const float* micR,
                      float* dstL, float* dstR,
                      uint32_t numFrames,
                      const ControlRoomParams& params) noexcept {
        if (!dawL || !dawR || !dstL || !dstR || numFrames == 0 || m_fdnBuf.empty()) return;

        const float targetMonMix = std::clamp(params.directMonitorMix, 0.0f, 1.0f);
        const float targetVerbWet = (targetMonMix > 0.001f) ? (std::clamp(params.monitorReverbAmount, 0.0f, 1.0f) * 0.42f) : 0.0f;
        const float targetXfeed = params.crossfeedEnabled ? 1.0f : 0.0f;

        if (!m_hasRamped) {
            m_hasRamped = true;
            m_monMixSmooth = targetMonMix;
            m_verbWetSmooth = targetVerbWet;
            m_xfeedSmooth = targetXfeed;
        }

        if (params.backingHeadroomDb != m_cachedHeadroomDb) {
            m_cachedHeadroomDb = params.backingHeadroomDb;
            m_cachedHeadroomLin = (params.backingHeadroomDb < -0.05f)
                ? std::pow(10.0f, std::clamp(params.backingHeadroomDb, -24.0f, 0.0f) * 0.05f)
                : 1.0f;
        }

        const bool anyActive = (targetMonMix > 0.001f || m_monMixSmooth > 0.001f
                             || targetVerbWet > 0.001f || m_verbWetSmooth > 0.001f
                             || targetXfeed > 0.001f || m_xfeedSmooth > 0.001f);
        if (!anyActive) {
            if (dstL != dawL) {
                for (uint32_t i = 0; i < numFrames; ++i) dstL[i] = dawL[i];
            }
            if (dstR != dawR) {
                for (uint32_t i = 0; i < numFrames; ++i) dstR[i] = dawR[i];
            }
            return;
        }

        const bool hasCarve = (targetMonMix > 0.001f) && params.cueCarveEnabled;

        for (uint32_t i = 0; i < numFrames; ++i) {
            m_monMixSmooth  += (targetMonMix - m_monMixSmooth) * m_rampCoeff;
            m_verbWetSmooth += (targetVerbWet - m_verbWetSmooth) * m_rampCoeff;
            m_xfeedSmooth   += (targetXfeed - m_xfeedSmooth) * m_rampCoeff;

            float dL = dawL[i];
            float dR = dawR[i];
            const float mL = micL ? micL[i] : 0.0f;
            const float mR = micR ? micR[i] : mL;

            // 1. Anti-Bleed Dynamic Cue Carve (-1.8 dB @ 2.2 kHz on backing track while singing)
            if (hasCarve) {
                const float vAbs = std::max(std::fabs(mL), std::fabs(mR));
                m_cueEnv += (vAbs - m_cueEnv) * ((vAbs > m_cueEnv) ? m_cueAtkCoeff : m_cueRelCoeff);
                const float targetDepth = (m_cueEnv > 0.015f) ? std::clamp((m_cueEnv - 0.015f) * 8.0f, 0.0f, 1.0f) : 0.0f;
                m_cueDepthSmooth += (targetDepth - m_cueDepthSmooth) * m_cueAtkCoeff;
                if (m_cueDepthSmooth > 0.005f) {
                    const float carvedL = m_cueBell[0].Process(dL);
                    const float carvedR = m_cueBell[1].Process(dR);
                    dL = dL + (carvedL - dL) * m_cueDepthSmooth;
                    dR = dR + (carvedR - dR) * m_cueDepthSmooth;
                }
            }

            // 2. Monitor-Only Comfort Chamber v2 (Velvet ER + 8x8 AVX2 Householder FDN + 0.35 Hz LFO)
            float monOutL = mL;
            float monOutR = mR;
            if (m_verbWetSmooth > 0.0005f) {
                const float monoIn = 0.5f * (mL + mR);

                // Velvet-noise early reflections (0-15 ms)
                m_erBuf[m_erWriteIdx] = monoIn;
                float erSumL = 0.0f, erSumR = 0.0f;
                for (int t = 0; t < 12; ++t) {
                    const uint32_t rIdx = (m_erWriteIdx + kErMax - m_erDelays[t]) & (kErMax - 1u);
                    const float tap = m_erBuf[rIdx] * m_erSigns[t];
                    if ((t & 1) == 0) erSumL += tap;
                    else              erSumR += tap;
                }
                m_erWriteIdx = (m_erWriteIdx + 1u) & (kErMax - 1u);

                // Fractional LFO delay modulation (0.35 Hz +/- 0.25 ms)
                m_lfoPhase += m_lfoPhaseInc;
                if (m_lfoPhase > 6.2831853f) m_lfoPhase -= 6.2831853f;
                const float lfoSin = std::sin(m_lfoPhase);

                alignas(32) float x[8];
                for (int k = 0; k < 8; ++k) {
                    const float modDelay = std::max(8.0f, m_fdnDelayBase[k] + m_lfoModSamples * ((k & 1) ? -lfoSin : lfoSin));
                    const uint32_t dInt = static_cast<uint32_t>(modDelay);
                    const float frac = modDelay - static_cast<float>(dInt);
                    const uint32_t r0 = (m_fdnWriteIdx[k] + kFdnLineMax - dInt) & (kFdnLineMax - 1u);
                    const uint32_t r1 = (r0 - 1u) & (kFdnLineMax - 1u);
                    const float* line = &m_fdnBuf[static_cast<size_t>(k) * kFdnLineMax];
                    const float rawTap = line[r0] + (line[r1] - line[r0]) * frac;

                    // Dual-band RT60 damping: LF rumble stability + HF air rolloff
                    m_fdnLfState[k] += (rawTap - m_fdnLfState[k]) * m_fdnLfDampCoeff;
                    const float bandTap = rawTap - 0.12f * m_fdnLfState[k];
                    m_fdnHfState[k] += (bandTap - m_fdnHfState[k]) * m_fdnHfDampCoeff;
                    x[k] = m_fdnHfState[k];
                }

                alignas(32) float scattered[8];
                for (int k = 0; k < 8; ++k) scattered[k] = x[k];
                vocal::HouseholderScatter8(scattered);

                constexpr float kDecay = 0.72f;
                for (int k = 0; k < 8; ++k) {
                    float* line = &m_fdnBuf[static_cast<size_t>(k) * kFdnLineMax];
                    line[m_fdnWriteIdx[k]] = monoIn + scattered[k] * kDecay;
                    m_fdnWriteIdx[k] = (m_fdnWriteIdx[k] + 1u) & (kFdnLineMax - 1u);
                }
                const float wetL = 0.35f * (x[0] - x[1] + x[2] - x[3]) + 0.045f * erSumL;
                const float wetR = 0.35f * (x[4] + x[5] - x[6] - x[7]) + 0.045f * erSumR;
                monOutL += wetL * m_verbWetSmooth;
                monOutR += wetR * m_verbWetSmooth;
            }

            if (m_monMixSmooth > 0.05f && m_cachedHeadroomLin < 0.994f) {
                dL *= m_cachedHeadroomLin;
                dR *= m_cachedHeadroomLin;
            }

            float mixL = dL + monOutL * m_monMixSmooth;
            float mixR = dR + monOutR * m_monMixSmooth;

            // 3. Spagnol Spherical-Head + Pinna-Notch Externalizer (280 us ITD, 700 Hz shadow, 7.8 kHz notch, 11.5 kHz elev)
            if (m_xfeedSmooth > 0.001f) {
                m_xfeedBufL[m_xfeedWriteIdx] = mixL;
                m_xfeedBufR[m_xfeedWriteIdx] = mixR;
                const uint32_t rIdx = (m_xfeedWriteIdx + kXfeedMax - m_xfeedDelayLen) & (kXfeedMax - 1u);
                const float delL = m_xfeedBufL[rIdx];
                const float delR = m_xfeedBufR[rIdx];
                m_xfeedWriteIdx = (m_xfeedWriteIdx + 1u) & (kXfeedMax - 1u);

                m_xfeedLpL += (delL - m_xfeedLpL) * m_xfeedLpCoeff;
                m_xfeedLpR += (delR - m_xfeedLpR) * m_xfeedLpCoeff;

                const float extL = m_elevCue[0].Process(m_pinnaNotch[0].Process(mixL * 0.86f + m_xfeedLpR * 0.28f));
                const float extR = m_elevCue[1].Process(m_pinnaNotch[1].Process(mixR * 0.86f + m_xfeedLpL * 0.28f));
                mixL = mixL + (extL - mixL) * m_xfeedSmooth;
                mixR = mixR + (extR - mixR) * m_xfeedSmooth;
            }

            dstL[i] = FiniteOrZero(mixL);
            dstR[i] = FiniteOrZero(mixR);
        }
    }

private:
    static constexpr float    kPi         = 3.14159265358979323846f;
    static constexpr uint32_t kFdnLineMax = 16384u;
    static constexpr uint32_t kErMax      = 4096u;
    static constexpr uint32_t kXfeedMax   = 256u;

    struct SecBiquad {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;
        void Reset() noexcept { z1 = z2 = 0.0f; }
        inline float Process(float x) noexcept {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void SetPeaking(float freqHz, float q, float gainDb, float fs) noexcept {
            const float A = std::pow(10.0f, gainDb / 40.0f);
            const float w0 = 2.0f * kPi * std::min(freqHz, fs * 0.45f) / fs;
            const float cs = std::cos(w0);
            const float alpha = std::sin(w0) / (2.0f * std::max(q, 0.35f));
            const float invA0 = 1.0f / (1.0f + alpha / A);
            b0 = (1.0f + alpha * A) * invA0; b1 = (-2.0f * cs) * invA0; b2 = (1.0f - alpha * A) * invA0;
            a1 = (-2.0f * cs) * invA0;       a2 = (1.0f - alpha / A) * invA0;
        }
    };

    static float FiniteOrZero(float x) noexcept {
        uint32_t u = 0;
        const auto* p = reinterpret_cast<const unsigned char*>(&x);
        u = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
          | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        if ((u & 0x7F800000u) == 0x7F800000u) return 0.0f;
        return x;
    }

    double m_fs = 48000.0;
    std::vector<float> m_fdnBuf;
    std::vector<float> m_erBuf;
    std::array<float, 8>    m_fdnDelayBase{};
    std::array<uint32_t, 8> m_fdnWriteIdx{};
    std::array<float, 8>    m_fdnHfState{};
    std::array<float, 8>    m_fdnLfState{};
    float m_fdnHfDampCoeff = 0.45f;
    float m_fdnLfDampCoeff = 0.01f;
    float m_lfoPhase = 0.0f;
    float m_lfoPhaseInc = 0.0f;
    float m_lfoModSamples = 12.0f;

    std::array<uint32_t, 12> m_erDelays{};
    std::array<float, 12>    m_erSigns{};
    uint32_t m_erWriteIdx = 0u;

    std::array<float, kXfeedMax> m_xfeedBufL{};
    std::array<float, kXfeedMax> m_xfeedBufR{};
    uint32_t m_xfeedDelayLen = 14u;
    uint32_t m_xfeedWriteIdx = 0u;
    float m_xfeedLpCoeff = 0.08f;
    float m_xfeedLpL = 0.0f;
    float m_xfeedLpR = 0.0f;
    SecBiquad m_pinnaNotch[2];
    SecBiquad m_elevCue[2];

    float m_rampCoeff = 0.0014f;
    bool  m_hasRamped = false;
    float m_monMixSmooth = 0.0f;
    float m_verbWetSmooth = 0.0f;
    float m_xfeedSmooth = 0.0f;
    float m_cachedHeadroomDb = -999.0f;
    float m_cachedHeadroomLin = 1.0f;

    float m_cueAtkCoeff = 0.002f;
    float m_cueRelCoeff = 0.00015f;
    float m_cueEnv = 0.0f;
    float m_cueDepthSmooth = 0.0f;
    SecBiquad m_cueBell[2];
};

} // namespace stoneridge
