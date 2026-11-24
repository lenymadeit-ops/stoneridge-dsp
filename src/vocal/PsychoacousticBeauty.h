#pragma once

// Psychoacoustic Beauty v7:
//   1. ISO 226:2023 dynamic equal-loudness contour (LUFS-driven ZDF shelves @ 95 Hz / 11.5 kHz).
//   2. Bark/ERB pure 2f quadrature harmonic chest (LR4 70-260 Hz -> 4-section Hilbert IIR -> y_I^2 - y_Q^2)
//      with 260-520 Hz ERB masking gate, plus smoothed Silk Air harmonic shimmer (12-19 kHz).
//   3. 3-band (2.4, 3.6, 5.2 kHz) analytic Complex Teager-Kaiser (CTKEO) dynamic de-harsher.
//   4. Parallel Motown/NY glue bus (140 Hz - 9 kHz -> 4:1 micro-leveler + Adaa2MolecularShaper).
// Zero heap allocations, zero per-sample trig/pow, bit-transparent after 50 ms sleeper bypass.

#include "vocal/Adaa2MolecularShaper.h"
#include "vocal/SimdKernels.h"
#include "vocal/WdfPultecEq.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace stoneridge::vocal {

struct BeautyParams {
    float beautyAmount      = 0.0f;
    float contourAmt        = 1.0f;
    float harmonicSculptAmt = 1.0f;
    float deHarshAmt        = 1.0f;
    float parallelGlueAmt   = 0.15f;
    float shortTermLufs     = -18.0f;
};

class PsychoacousticBeauty {
public:
    void Prepare(double sampleRate) noexcept {
        m_fs = static_cast<float>((sampleRate > 1000.0) ? sampleRate : 48000.0);
        constexpr float kPi = 3.14159265358979323846f;

        // 1. Precomputed ISO 226 ZDF shelves (blended dynamically -> 0 per-sample trig/pow)
        m_isoLowShelf.Prepare(m_fs);
        m_isoAirShelf.Prepare(m_fs);

        // 2. LR4 70-260 Hz chest bandpass, 520 Hz ERB masking band, & 520 Hz 2f cleanup LPF
        m_chestHp1.SetHpf(70.0f, m_fs);
        m_chestHp2.SetHpf(70.0f, m_fs);
        m_chestLp1.SetLowpass(260.0f, m_fs);
        m_chestLp2.SetLowpass(260.0f, m_fs);
        m_erbMaskLp.SetLowpass(520.0f, m_fs);
        m_quad2fLp.SetLowpass(520.0f, m_fs);

        // Matched 4-section IIR Hilbert quadrature pair (bilinearly warped from 48 kHz reference)
        constexpr double kRefI[4] = { 0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737 };
        constexpr double kRefQ[4] = { 0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278 };
        for (int k = 0; k < 4; ++k) {
            const double lamI = 48000.0 * (1.0 - kRefI[k]) / (1.0 + kRefI[k]);
            const double lamQ = 48000.0 * (1.0 - kRefQ[k]) / (1.0 + kRefQ[k]);
            m_hilbertCi[k] = static_cast<float>((static_cast<double>(m_fs) - lamI) / (static_cast<double>(m_fs) + lamI));
            m_hilbertCq[k] = static_cast<float>((static_cast<double>(m_fs) - lamQ) / (static_cast<double>(m_fs) + lamQ));
        }

        // 3. Silk Air bandpass filters (6-12 kHz excitation -> 12-19 kHz harmonic air)
        m_airInHp.SetHpf(6000.0f, m_fs);
        m_airInLp.SetLowpass(12000.0f, m_fs);
        m_airOutHp.SetHpf(12000.0f, m_fs);
        m_airOutLp.SetLowpass(std::min(19000.0f, m_fs * 0.44f), m_fs);

        // 4. 3-band analytic CTKEO pharyngeal resonators (2.4, 3.6, 5.2 kHz) & ZDF dynamic bells
        constexpr float kHarshHz[3] = { 2400.0f, 3600.0f, 5200.0f };
        for (int b = 0; b < 3; ++b) {
            const float fc = std::min(kHarshHz[b], m_fs * 0.42f);
            const float bw = fc / 3.2f;
            const float r = std::exp(-kPi * bw / m_fs);
            const float w = 2.0f * kPi * fc / m_fs;
            m_ctR[b] = r;
            m_ctInGain[b] = 1.0f - r;
            m_ctCos[b] = std::cos(w);
            m_ctSin[b] = std::sin(w);
            m_harshBell[b].Prepare(m_fs);
            m_harshFreq[b] = fc;
        }

        // 5. Parallel NY glue bus bandpass (140 Hz - 9 kHz) + Adaa2MolecularShaper
        m_nyHp.SetHpf(140.0f, m_fs);
        m_nyLp.SetLowpass(std::min(9000.0f, m_fs * 0.42f), m_fs);
        m_nyMol.Prepare(m_fs);

        // Time constants
        m_dcCoeff = 1.0f - std::exp(-2.0f * kPi * 10.0f / m_fs);
        m_envFast = 1.0f - std::exp(-1.0f / (0.004f * m_fs));
        m_envRel = 1.0f - std::exp(-1.0f / (0.080f * m_fs));
        // CTKEO attack (~16 ms) rejects brief <1 ms broadband consonant clicks while catching sustained harshness
        m_ctAtk16 = 1.0f - std::exp(-16.0f / (0.016f * m_fs));
        m_ctRel16 = 1.0f - std::exp(-16.0f / (0.065f * m_fs));

        m_fade.Prepare(sampleRate);
        m_initialized = false;
        Reset();
    }

    void Reset() noexcept {
        m_isoLowShelf.Reset();
        m_isoAirShelf.Reset();
        m_chestHp1.Reset(); m_chestHp2.Reset();
        m_chestLp1.Reset(); m_chestLp2.Reset();
        m_erbMaskLp.Reset();
        m_quad2fLp.Reset();
        for (int k = 0; k < 4; ++k) {
            m_hiX1[k] = m_hiX2[k] = m_hiY1[k] = m_hiY2[k] = 0.0f;
            m_hqX1[k] = m_hqX2[k] = m_hqY1[k] = m_hqY2[k] = 0.0f;
        }
        m_hilbertQDelay = 0.0f;
        m_dcQuadX1 = m_dcQuadY1 = 0.0f;
        m_chestEnv = m_maskEnv = 0.0f;
        m_airInHp.Reset(); m_airInLp.Reset();
        m_airOutHp.Reset(); m_airOutLp.Reset();
        m_airGateEnv = m_airSibEnv = 0.0f;
        for (int b = 0; b < 3; ++b) {
            m_ctRe[b] = m_ctIm[b] = 0.0f;
            m_ctPrev1Re[b] = m_ctPrev1Im[b] = 0.0f;
            m_ctPrev2Re[b] = m_ctPrev2Im[b] = 0.0f;
            m_ctEnv[b] = 0.0f;
            m_ctGainDb[b] = 0.0f;
            m_harshBell[b].Reset();
        }
        m_nyHp.Reset(); m_nyLp.Reset();
        m_nyMol.Reset();
        m_glueEnv = 0.0f;
        m_ctCtrlCtr = 0u;
        m_fade.Reset();
        m_initialized = false;
    }

    inline float Process(float x, const BeautyParams& p) noexcept {
        const bool on = p.beautyAmount > 0.001f;
        if (!m_initialized) {
            m_initialized = true;
            m_fade.ForceEnabled(on);
        } else {
            m_fade.SetEnabled(on);
        }
        const float gFade = m_fade.Next();
        if (m_fade.IsAsleep()) return x;

        const float amt = std::clamp(p.beautyAmount, 0.0f, 1.0f);
        const float harmAmt = amt * std::clamp(p.harmonicSculptAmt, 0.0f, 1.0f);
        const float deHarshAmt = amt * std::clamp(p.deHarshAmt, 0.0f, 1.0f);
        const float nyAmt = amt * std::clamp(p.parallelGlueAmt, 0.0f, 0.25f);

        // 1. ISO 226:2023 Dynamic Equal-Loudness Contour (fixed ZDF shelves blended smoothly by LUFS deficit)
        const float loudDeficit = std::clamp((-18.0f - p.shortTermLufs) / 18.0f, 0.0f, 1.0f);
        const float contourBlend = loudDeficit * amt * std::clamp(p.contourAmt, 0.0f, 1.0f);
        float shaped = x;
        if (contourBlend > 0.002f) {
            const float lowShelved = m_isoLowShelf.ProcessLowShelf(shaped, 95.0f, 0.707f, 2.5f);
            const float airShelved = m_isoAirShelf.ProcessHighShelf(lowShelved, 11500.0f, 0.707f, 1.8f);
            shaped = shaped + (airShelved - shaped) * contourBlend;
        }

        // 2. Bark/ERB Pure 2f Quadrature Chest + Smoothed Silk Air
        float y = shaped;
        if (harmAmt > 0.002f) {
            const float chestBp = m_chestLp2.Process(m_chestLp1.Process(m_chestHp2.Process(m_chestHp1.Process(x))));
            const float erbMask = m_erbMaskLp.Process(x) - chestBp; // 260-520 Hz masking mud
            const float absChest = std::fabs(chestBp);
            const float absMask = std::fabs(erbMask);
            m_chestEnv += (absChest - m_chestEnv) * ((absChest > m_chestEnv) ? m_envFast : m_envRel);
            m_maskEnv += (absMask - m_maskEnv) * ((absMask > m_maskEnv) ? m_envFast : m_envRel);
            const float erbGate = m_chestEnv / (m_chestEnv + 0.65f * m_maskEnv + 1.0e-4f);

            float xI = chestBp;
            float xQ = chestBp;
            for (int k = 0; k < 4; ++k) {
                const float ci = m_hilbertCi[k];
                const float yi = ci * (xI - m_hiY2[k]) + m_hiX2[k];
                m_hiX2[k] = m_hiX1[k]; m_hiX1[k] = xI;
                m_hiY2[k] = m_hiY1[k]; m_hiY1[k] = yi;
                xI = yi;

                const float cq = m_hilbertCq[k];
                const float yq = cq * (xQ - m_hqY2[k]) + m_hqX2[k];
                m_hqX2[k] = m_hqX1[k]; m_hqX1[k] = xQ;
                m_hqY2[k] = m_hqY1[k]; m_hqY1[k] = yq;
                xQ = yq;
            }
            const float xQAligned = m_hilbertQDelay;
            m_hilbertQDelay = xQ;

            const float quadMag = std::sqrt(xI * xI + xQAligned * xQAligned + 1.0e-8f);
            const float pure2f = (xI * xI - xQAligned * xQAligned) / std::max(0.18f, quadMag);
            const float dcFree2f = pure2f - m_dcQuadX1 + (1.0f - m_dcCoeff) * m_dcQuadY1;
            m_dcQuadX1 = pure2f;
            m_dcQuadY1 = dcFree2f;
            const float smooth2f = m_quad2fLp.Process(dcFree2f);

            // Smoothed Silk Air (6-12 kHz -> 12-19 kHz) with continuous noise gate & sibilance duck
            const float airIn = m_airInLp.Process(m_airInHp.Process(x));
            const float absAir = std::fabs(airIn);
            m_airGateEnv += (absAir - m_airGateEnv) * ((absAir > m_airGateEnv) ? m_envFast : m_envRel);
            m_airSibEnv += (absAir - m_airSibEnv) * ((absAir > m_airSibEnv) ? m_envFast : m_envRel);
            const float noiseOpen = std::clamp((m_airGateEnv - 0.004f) / 0.008f, 0.0f, 1.0f);
            const float sibDuck = 1.0f - std::clamp((m_airSibEnv - 0.08f) / 0.12f, 0.0f, 1.0f);
            const float rawAir2f = m_airOutLp.Process(m_airOutHp.Process(airIn * airIn * ((airIn >= 0.0f) ? 1.0f : -1.0f)));
            const float silkAir = rawAir2f * 0.25f * noiseOpen * sibDuck;

            y += (smooth2f * 0.16f * erbGate + silkAir) * harmAmt;
        }

        // 3. 3-Band Analytic CTKEO Pharyngeal De-Harsher (resonators @ fs, sqrt/env decimated 16x)
        if (deHarshAmt > 0.005f) {
            const bool ctrlTick = ((m_ctCtrlCtr++ & 15u) == 0u);
            for (int b = 0; b < 3; ++b) {
                const float reNext = m_ctInGain[b] * y + m_ctR[b] * (m_ctCos[b] * m_ctRe[b] - m_ctSin[b] * m_ctIm[b]);
                const float imNext = m_ctR[b] * (m_ctSin[b] * m_ctRe[b] + m_ctCos[b] * m_ctIm[b]);
                if (ctrlTick) {
                    const float magSq1 = m_ctPrev1Re[b] * m_ctPrev1Re[b] + m_ctPrev1Im[b] * m_ctPrev1Im[b];
                    const float cross = reNext * m_ctPrev2Re[b] + imNext * m_ctPrev2Im[b];
                    const float psiC = std::max(0.0f, magSq1 - cross);
                    const float ctAmp = std::sqrt(psiC);
                    m_ctEnv[b] += (ctAmp - m_ctEnv[b]) * ((ctAmp > m_ctEnv[b]) ? m_ctAtk16 : m_ctRel16);
                    const float excess = std::max(0.0f, (m_ctEnv[b] - 0.022f) * 32.0f);
                    const float targetCutDb = -std::clamp(excess * 5.0f * deHarshAmt, 0.0f, 6.0f);
                    if (std::fabs(targetCutDb - m_ctGainDb[b]) > 0.15f) {
                        m_ctGainDb[b] = targetCutDb;
                    }
                }
                m_ctPrev2Re[b] = m_ctPrev1Re[b]; m_ctPrev2Im[b] = m_ctPrev1Im[b];
                m_ctPrev1Re[b] = reNext;         m_ctPrev1Im[b] = imNext;
                m_ctRe[b] = reNext;              m_ctIm[b] = imNext;

                if (m_ctGainDb[b] < -0.05f) {
                    y = m_harshBell[b].ProcessBell(y, m_harshFreq[b], 2.8f, m_ctGainDb[b]);
                }
            }
        }

        // 4. Parallel Motown / NY Glue Bus (140 Hz - 9 kHz -> 4:1 micro-leveler + Adaa2MolecularShaper)
        if (nyAmt > 0.001f) {
            const float nyBand = m_nyLp.Process(m_nyHp.Process(y));
            const float absNy = std::fabs(nyBand);
            m_glueEnv += (absNy - m_glueEnv) * ((absNy > m_glueEnv) ? m_envFast : m_envRel);
            const float over = std::max(0.0f, m_glueEnv - 0.08f);
            const float lvlGain = 1.0f / (1.0f + 4.0f * over);
            const float nySquashed = m_nyMol.Process(std::clamp(nyBand * lvlGain * 1.4f, -1.2f, 1.2f));
            y += (nySquashed - nyBand) * nyAmt;
        } else {
            m_nyMol.TrackIdle(y);
        }

        return std::clamp(x + (y - x) * gFade, -1.0f, 1.0f);
    }

private:
    struct Sec2 {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;
        void Reset() noexcept { z1 = z2 = 0.0f; }
        inline float Process(float x) noexcept {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void SetLowpass(float fc, float fs) noexcept {
            constexpr float kPi = 3.14159265358979323846f;
            const float w0 = 2.0f * kPi * std::clamp(fc, 20.0f, fs * 0.45f) / fs;
            const float cs = std::cos(w0);
            const float alpha = std::sin(w0) / 1.41421356f;
            const float inv = 1.0f / (1.0f + alpha);
            b0 = (1.0f - cs) * 0.5f * inv; b1 = (1.0f - cs) * inv; b2 = b0;
            a1 = (-2.0f * cs) * inv; a2 = (1.0f - alpha) * inv;
        }
        void SetHpf(float fc, float fs) noexcept {
            constexpr float kPi = 3.14159265358979323846f;
            const float w0 = 2.0f * kPi * std::clamp(fc, 20.0f, fs * 0.45f) / fs;
            const float cs = std::cos(w0);
            const float alpha = std::sin(w0) / 1.41421356f;
            const float inv = 1.0f / (1.0f + alpha);
            b0 = (1.0f + cs) * 0.5f * inv; b1 = -(1.0f + cs) * inv; b2 = b0;
            a1 = (-2.0f * cs) * inv; a2 = (1.0f - alpha) * inv;
        }
    };

    float m_fs = 48000.0f;
    bool  m_initialized = false;
    float m_dcCoeff = 0.001f;
    float m_envFast = 0.01f, m_envRel = 0.001f;
    float m_ctAtk16 = 0.032f, m_ctRel16 = 0.0064f;
    uint32_t m_ctCtrlCtr = 0u;

    ZdfTptFilter m_isoLowShelf, m_isoAirShelf;
    Sec2  m_chestHp1, m_chestHp2, m_chestLp1, m_chestLp2, m_erbMaskLp, m_quad2fLp;
    float m_hilbertCi[4]{}, m_hilbertCq[4]{};
    float m_hiX1[4]{}, m_hiX2[4]{}, m_hiY1[4]{}, m_hiY2[4]{};
    float m_hqX1[4]{}, m_hqX2[4]{}, m_hqY1[4]{}, m_hqY2[4]{};
    float m_hilbertQDelay = 0.0f;
    float m_dcQuadX1 = 0.0f, m_dcQuadY1 = 0.0f;
    float m_chestEnv = 0.0f, m_maskEnv = 0.0f;

    Sec2  m_airInHp, m_airInLp, m_airOutHp, m_airOutLp;
    float m_airGateEnv = 0.0f, m_airSibEnv = 0.0f;

    float m_ctR[3]{}, m_ctInGain[3]{}, m_ctCos[3]{}, m_ctSin[3]{};
    float m_ctRe[3]{}, m_ctIm[3]{}, m_ctPrev1Re[3]{}, m_ctPrev1Im[3]{}, m_ctPrev2Re[3]{}, m_ctPrev2Im[3]{};
    float m_ctEnv[3]{}, m_ctGainDb[3]{}, m_harshFreq[3]{ 2400.0f, 3600.0f, 5200.0f };
    ZdfTptFilter m_harshBell[3];

    Sec2 m_nyHp, m_nyLp;
    Adaa2MolecularShaper m_nyMol;
    float m_glueEnv = 0.0f;
    StageCrossfadeSleeper m_fade;
};

} // namespace stoneridge::vocal
