#pragma once

// Shared B+ reservoir and continuous-time Koren triode plate physics with
// asymmetric grid-leak bias recovery and sample-rate-invariant DC blocking.
// Sample-rate poles are computed in Prepare(). No heap, no locks.

#include "vocal/Adaa2MolecularShaper.h"
#include "vocal/SimdKernels.h"

#include <algorithm>
#include <cmath>

namespace stoneridge::vocal {

class PowerSupplyRail {
public:
    void Prepare(double sampleRate) noexcept {
        const float fs = static_cast<float>((sampleRate > 1000.0) ? sampleRate : 48000.0);
        m_sag = 1.0f - std::exp(-1.0f / (0.018f * fs));
        m_rec = 1.0f - std::exp(-1.0f / (0.085f * fs));
        Reset();
    }

    void Reset() noexcept { m_droop = 0.0f; }

    // current is 0..1 instantaneous draw. Returns B+ scale in about 0.965..1.
    inline float Process(float current) noexcept {
        const float c = std::clamp(current, 0.0f, 1.0f);
        const float coeff = (c > m_droop) ? m_sag : m_rec;
        m_droop += (c - m_droop) * coeff;
        return 1.0f - 0.035f * m_droop;
    }

    float Droop() const noexcept { return m_droop; }

private:
    float m_sag = 0.001f;
    float m_rec = 0.0002f;
    float m_droop = 0.0f;
};

class TriodeGridBiasStage {
public:
    void Prepare(double sampleRate, float gridGain = 1.35f) noexcept {
        const float fs = static_cast<float>((sampleRate > 1000.0) ? sampleRate : 48000.0);
        m_gridGain = std::clamp(gridGain, 0.35f, 3.5f);
        // Asymmetric grid-leak network: fast positive grid conduction (~16 ms), slow RC leak (~52 ms).
        m_gridAtkCoeff = 1.0f - std::exp(-1.0f / (0.016f * fs));
        m_gridRelCoeff = 1.0f - std::exp(-1.0f / (0.052f * fs));
        m_dcCoeff = 1.0f - std::exp(-2.0f * 3.14159265358979323846f * 7.5f / fs);
        m_shaper.Prepare(sampleRate);

        // Calibrate exact quiescent point & small-signal unity slope for 12AX7A Koren model
        // including the loaded plate swing (-18.0 * gridSignal).
        constexpr float kVpk0 = 165.0f;
        constexpr float kVgk0 = -1.60f;
        m_ip0 = EvalKorenPlate(kVpk0, kVgk0);
        const float ipHi = EvalKorenPlate(kVpk0 - 18.0f * 0.002f, kVgk0 + 0.002f);
        const float ipLo = EvalKorenPlate(kVpk0 + 18.0f * 0.002f, kVgk0 - 0.002f);
        const float rawSlope = (ipHi - ipLo) / 0.004f;
        m_normScale = (rawSlope > 1.0e-7f) ? (1.0f / (rawSlope * m_gridGain)) : 1.0f;

        for (int i = 0; i <= kLutSize; ++i) {
            const float g = -4.0f + 8.0f * (static_cast<float>(i) / static_cast<float>(kLutSize));
            const float vpk = 165.0f - 18.0f * std::clamp(g, -2.5f, 2.5f);
            m_plateLut[i] = (EvalKorenPlate(vpk, kVgk0 + g) - m_ip0) * m_normScale;
        }

        Reset();
    }

    void Reset() noexcept {
        m_gridBias = 0.0f;
        m_dcX1 = 0.0f;
        m_dcY1 = 0.0f;
        m_shaper.Reset();
    }

    inline float Process(float x, float bPlus, float driveScale = 1.0f) noexcept {
        const float bScale = std::clamp(bPlus, 0.70f, 1.05f);
        const float dScale = std::clamp(driveScale, 0.25f, 3.0f);
        const float vGridIn = x * m_gridGain * dScale;

        // Grid conduction draws negative bias shift when instantaneous grid goes > -0.15 V.
        const float gridSignal = vGridIn + m_gridBias;
        const float gridOver = std::max(0.0f, gridSignal - 1.45f);
        const float targetBias = -std::min(1.6f, gridOver * 0.75f);
        const float gCoeff = (targetBias < m_gridBias) ? m_gridAtkCoeff : m_gridRelCoeff;
        m_gridBias += (targetBias - m_gridBias) * gCoeff;

        // Common-cathode Koren plate current via 257-entry interpolated LUT with B+ sag modulation.
        const float clampedSig = std::clamp(gridSignal, -4.0f, 3.9999f);
        const float pos = (clampedSig + 4.0f) * (static_cast<float>(kLutSize) / 8.0f);
        const int idx = static_cast<int>(pos);
        const float frac = pos - static_cast<float>(idx);
        const float lutVal = m_plateLut[idx] + (m_plateLut[idx + 1] - m_plateLut[idx]) * frac;
        const float invDScale = 1.0f / dScale;
        const float sagGain = 1.0f - 1.4f * (1.0f - bScale);
        const float rawAudio = lutVal * sagGain * invDScale;

        // Sample-rate-invariant DC blocker (~7.5 Hz HPF).
        const float dcFree = rawAudio - m_dcX1 + (1.0f - m_dcCoeff) * m_dcY1;
        m_dcX1 = rawAudio;
        m_dcY1 = dcFree;

        // Subtle molecular restoring-torque polish only at driven peaks (transparent when quiet).
        if (std::fabs(dcFree) < 0.09f) {
            m_shaper.TrackIdle(dcFree);
            return dcFree;
        }
        const float shaped = m_shaper.Process(std::clamp(dcFree, -1.25f, 1.25f));
        const float satMix = std::clamp((std::fabs(dcFree) - 0.09f) * 0.45f, 0.0f, 0.35f);
        return dcFree + (shaped - dcFree) * satMix;
    }

private:
    static inline float Softplus(float z) noexcept {
        if (z > 14.0f) return z;
        if (z < -14.0f) return 0.0f;
        return 0.69314718f * FastLog2(1.0f + FastExp2(1.44269504f * z));
    }

    static inline float EvalKorenPlate(float vpk, float vgk) noexcept {
        // 12AX7A Koren parameters: mu = 100, Kp = 600, Kvb = 300, Kg1 = 1060, Ex = 1.4.
        constexpr float kMu = 100.0f;
        constexpr float kKp = 600.0f;
        constexpr float kKvb = 300.0f;
        constexpr float kInvKg1 = 1.0f / 1060.0f;
        const float vp = std::max(5.0f, vpk);
        const float z = kKp * ((1.0f / kMu) + vgk / std::sqrt(kKvb + vp * vp));
        const float e1 = (vp / kKp) * Softplus(z);
        if (e1 <= 1.0e-7f) return 0.0f;
        return FastExp2(1.4f * FastLog2(e1)) * kInvKg1;
    }

    float m_gridGain = 1.35f;
    float m_gridAtkCoeff = 0.001f;
    float m_gridRelCoeff = 0.0004f;
    float m_dcCoeff = 0.001f;
    float m_ip0 = 0.001f;
    float m_normScale = 1.0f;
    float m_gridBias = 0.0f;
    float m_dcX1 = 0.0f;
    float m_dcY1 = 0.0f;
    static constexpr int kLutSize = 256;
    float m_plateLut[kLutSize + 1]{};
    Adaa2MolecularShaper m_shaper;
};

} // namespace stoneridge::vocal
