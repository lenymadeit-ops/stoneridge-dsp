#pragma once

// True Wave Digital Filter (WDF) passive EQP-1A bridge (boost + cut interacting RC network
// plus WDF RLC 12 kHz air tank) and cached Cytomic SVF (ZdfTptFilter) with zero per-sample trig/pow.

#include <algorithm>
#include <cmath>

namespace stoneridge::vocal {

class WdfPultecEq {
public:
    void Prepare(double sampleRate) noexcept {
        m_fs = (sampleRate > 1000.0) ? sampleRate : 48000.0;
        m_cachedBoostDb = 999.0f;
        m_cachedCutDb = 999.0f;
        m_cachedBellDb = 999.0f;
        Reset();
        UpdateLowCoeffs(0.0f, 0.0f);
        UpdateBellCoeffs(0.0f);
    }

    void Reset() noexcept {
        m_wdfCboostState = 0.0f;
        m_wdfCboost2State = 0.0f;
        m_wdfCattenState = 0.0f;
        m_wdfLhiState = 0.0f;
        m_wdfChiState = 0.0f;
    }

    // Passive Pultec EQP-1A WDF ladder:
    // Positive boostDb applies the ~92 Hz resonant boost branch; simultaneous cutDb (< 0) engages
    // the ~245 Hz shunt attenuator branch so boost+cut interact nonlinearly via shared bridge loading.
    // If boostDb < 0 with cutDb >= -0.02f, applies clean low attenuation for negative consoleLowDb.
    inline float ProcessLow(float x, float boostDb, float cutDb) noexcept {
        const float b = std::clamp(boostDb, -10.0f, 10.0f);
        const float c = std::clamp(cutDb, -10.0f, 0.0f);
        if (b != m_cachedBoostDb || c != m_cachedCutDb) {
            UpdateLowCoeffs(b, c);
        }
        if (m_lowBypassed) return x;

        const float bCap1 = m_wdfCboostState;
        const float bCap1b = m_wdfCboost2State;
        const float bCap2 = m_wdfCattenState;

        const float vStage1 = m_lowG1 * x + m_lowK11 * bCap1 + m_lowK12 * bCap2;
        const float vNode1 = m_lowG1b * vStage1 + m_lowK1b * bCap1b;
        const float vNode2 = m_lowG2 * x + m_lowK22 * bCap2;

        m_wdfCboostState = 2.0f * vStage1 - bCap1;
        m_wdfCboost2State = 2.0f * vNode1 - bCap1b;
        m_wdfCattenState = 2.0f * vNode2 - bCap2;

        return m_lowDirect * x + m_lowOut1 * vNode1 + m_lowOut2 * vNode2;
    }

    // Passive 12 kHz RLC WDF series resonator tank + makeup gain (cached coefficients).
    inline float ProcessBell(float x, float gainDb) noexcept {
        const float g = std::clamp(gainDb, -10.0f, 10.0f);
        if (g != m_cachedBellDb) {
            UpdateBellCoeffs(g);
        }
        if (m_bellBypassed) return x;

        // Series RLC 1-port reflected wave: b_series = -(b_L + b_C) = -(-sL + sC) = sL - sC
        // Using standard WDF series 1-port R-L-C wave variable formulation:
        const float bL = -m_wdfLhiState;
        const float bC = m_wdfChiState;
        const float aS = x;
        // Series junction incident-to-reflected wave with total port resistance Rt = R_L + R_C + R_bw:
        const float sumB = bL + bC;
        const float iLoop = (aS - sumB) * m_bellInvRt;
        // Update bilinear WDF wave states cleanly
        m_wdfLhiState = bL + m_bellGammaL * (aS - sumB);
        m_wdfChiState = bC + m_bellGammaC * (aS - sumB);

        const float bpOut = m_bellRbw * iLoop;
        return x + m_bellMix * bpOut;
    }

private:
    void UpdateLowCoeffs(float boostDb, float cutDb) noexcept {
        m_cachedBoostDb = boostDb;
        m_cachedCutDb = cutDb;

        float effBoostDb = boostDb;
        float effCutDb = cutDb;
        if (boostDb < 0.0f && cutDb > -0.02f) {
            effBoostDb = 0.0f;
            effCutDb = boostDb;
        }
        if (std::fabs(effBoostDb) < 0.02f && std::fabs(effCutDb) < 0.02f) {
            m_lowBypassed = true;
            return;
        }
        m_lowBypassed = false;

        // Coupled passive EQP-1A WDF time constants:
        // Boost low branch around ~92 Hz (2-pole cascaded passive slope above 95 Hz);
        // cut shunt branch around ~245 Hz so boost+cut yields a ~210 Hz dip.
        constexpr double kTwoPi = 6.28318530717958647692;
        const double wb = std::tan(0.5 * kTwoPi * 92.0 / m_fs);
        const double wb2 = std::tan(0.5 * kTwoPi * (92.0 / 0.35) / m_fs);
        const double wc = std::tan(0.5 * kTwoPi * 245.0 / m_fs);

        const double gB = std::pow(10.0, std::max(0.0, static_cast<double>(effBoostDb)) * 1.15 / 20.0);
        const double gC = std::pow(10.0, static_cast<double>(effCutDb) / 20.0);

        const double rSrc = 1.0;
        const double rBoostCap = 1.0 / std::max(wb * 2.0, 1.0e-5);
        const double rBoost2Cap = 1.0 / std::max(wb2 * 2.0, 1.0e-5);
        const double rCutCap = 1.0 / std::max(wc * 2.0, 1.0e-5);

        const double tapB = (gB - 1.0) / std::max(gB, 1.0);
        const double tapC = 1.0 - gC;
        const double loadCoupling = 0.24 * tapB * tapC;

        const double r1b = rSrc * (1.0 + 0.30 * tapC);
        const double alpha1 = 1.0 / (1.0 + rBoostCap / r1b);
        const double alpha1b = 1.0 / (1.0 + rBoost2Cap / rSrc);
        const double r1c = rSrc * (1.0 - 0.14 * tapB);
        const double alpha2 = 1.0 / (1.0 + rCutCap / r1c);

        m_lowG1 = static_cast<float>(alpha1);
        m_lowK11 = static_cast<float>(1.0 - alpha1);
        m_lowK12 = static_cast<float>(-loadCoupling * alpha1);

        m_lowG1b = static_cast<float>(alpha1b);
        m_lowK1b = static_cast<float>(1.0 - alpha1b);

        m_lowG2 = static_cast<float>(alpha2);
        m_lowK22 = static_cast<float>(1.0 - alpha2);

        const double directGain = 1.0 - 0.06 * tapB * tapC;
        const double boostFeed = (gB - 1.0) * (1.0 - 0.14 * tapC);
        const double cutFeed = -tapC;
        m_lowDirect = static_cast<float>(directGain);
        m_lowOut1 = static_cast<float>(boostFeed);
        m_lowOut2 = static_cast<float>(cutFeed);
    }

    void UpdateBellCoeffs(float gainDb) noexcept {
        m_cachedBellDb = gainDb;
        if (std::fabs(gainDb) < 0.02f) {
            m_bellBypassed = true;
            return;
        }
        m_bellBypassed = false;

        constexpr double kPi = 3.14159265358979323846;
        const double fc = std::min(12000.0, 0.44 * m_fs);
        const double Q = 0.85;
        const double w0 = std::tan(kPi * fc / m_fs);
        const double rl = Q / std::max(w0, 1.0e-5);
        const double rc = w0 * Q;
        const double rbw = 1.0;
        const double rt = rbw + rl + rc;

        m_bellRbw = static_cast<float>(rbw);
        m_bellInvRt = static_cast<float>(1.0 / rt);
        m_bellGammaL = static_cast<float>(2.0 * rl / rt);
        m_bellGammaC = static_cast<float>(2.0 * rc / rt);

        const double linGain = std::pow(10.0, static_cast<double>(gainDb) / 20.0);
        m_bellMix = static_cast<float>(linGain - 1.0);
    }

    double m_fs = 48000.0;
    float m_cachedBoostDb = 999.0f;
    float m_cachedCutDb = 999.0f;
    float m_cachedBellDb = 999.0f;
    bool  m_lowBypassed = true;
    bool  m_bellBypassed = true;

    float m_lowG1 = 0.0f, m_lowK11 = 1.0f, m_lowK12 = 0.0f;
    float m_lowG1b = 0.0f, m_lowK1b = 1.0f;
    float m_lowG2 = 0.0f, m_lowK22 = 1.0f;
    float m_lowDirect = 1.0f, m_lowOut1 = 0.0f, m_lowOut2 = 0.0f;

    float m_bellRbw = 1.0f, m_bellInvRt = 0.33f;
    float m_bellGammaL = 0.0f, m_bellGammaC = 0.0f, m_bellMix = 0.0f;

    float m_wdfCboostState = 0.0f;
    float m_wdfCboost2State = 0.0f;
    float m_wdfCattenState = 0.0f;
    float m_wdfLhiState = 0.0f;
    float m_wdfChiState = 0.0f;
};

// Cached Cytomic TPT SVF (Bell, LowShelf, HighShelf) with exact symmetric boost/cut and optional inductor saturation.
class ZdfTptFilter {
public:
    enum class Mode { Bell, LowShelf, HighShelf };

    void Prepare(double sampleRate) noexcept {
        m_fs = (sampleRate > 1000.0) ? static_cast<float>(sampleRate) : 48000.0f;
        m_cachedFc = -1.0f;
        m_cachedQ = -1.0f;
        m_cachedGainDb = 999.0f;
        m_cachedSat = -1.0f;
        m_cachedMode = Mode::Bell;
        Reset();
    }

    void Reset() noexcept { m_ic1eq = m_ic2eq = 0.0f; }

    inline float ProcessBell(float x, float fc, float q, float gainDb, float inductorSat = 0.0f) noexcept {
        return ProcessMode(x, Mode::Bell, fc, q, gainDb, inductorSat);
    }

    inline float ProcessLowShelf(float x, float fc, float q, float gainDb, float inductorSat = 0.0f) noexcept {
        return ProcessMode(x, Mode::LowShelf, fc, q, gainDb, inductorSat);
    }

    inline float ProcessHighShelf(float x, float fc, float q, float gainDb, float inductorSat = 0.0f) noexcept {
        return ProcessMode(x, Mode::HighShelf, fc, q, gainDb, inductorSat);
    }

private:
    inline float ProcessMode(float x, Mode mode, float fc, float q, float gainDb, float sat) noexcept {
        if (mode != m_cachedMode || fc != m_cachedFc || q != m_cachedQ || gainDb != m_cachedGainDb || sat != m_cachedSat) {
            UpdateCoeffs(mode, fc, q, gainDb, sat);
        }
        if (m_bypassed) return x;

        const float v3 = x - m_ic2eq;
        const float v1 = m_a1 * m_ic1eq + m_a2 * v3;
        const float v2 = m_ic2eq + m_a2 * m_ic1eq + m_a3 * v3;
        m_ic1eq = 2.0f * v1 - m_ic1eq;
        m_ic2eq = 2.0f * v2 - m_ic2eq;
        const float v1S = (m_sat > 0.0f) ? (v1 / (1.0f + m_sat * v1 * v1)) : v1;
        return m_m0 * x + m_m1 * v1S + m_m2 * v2;
    }

    void UpdateCoeffs(Mode mode, float fc, float q, float gainDb, float sat) noexcept {
        m_cachedMode = mode;
        m_cachedFc = fc;
        m_cachedQ = q;
        m_cachedGainDb = gainDb;
        m_cachedSat = sat;

        if (std::fabs(gainDb) < 0.02f) {
            m_bypassed = true;
            return;
        }
        m_bypassed = false;
        m_sat = std::max(0.0f, sat);

        const double fs = static_cast<double>(m_fs);
        const double clampedFc = std::clamp(static_cast<double>(fc), 20.0, fs * 0.45);
        const double clampedQ = std::max(static_cast<double>(q), 0.30);
        const double A = std::pow(10.0, std::clamp(static_cast<double>(gainDb), -18.0, 18.0) / 40.0);

        double g = std::tan(3.14159265358979323846 * clampedFc / fs);
        double k = 1.0 / clampedQ;
        double m0 = 1.0, m1 = 0.0, m2 = 0.0;

        if (mode == Mode::Bell) {
            k = 1.0 / (clampedQ * A);
            m0 = 1.0;
            m1 = k * (A * A - 1.0);
            m2 = 0.0;
        } else if (mode == Mode::LowShelf) {
            g /= std::sqrt(A);
            m0 = 1.0;
            m1 = k * (A - 1.0);
            m2 = A * A - 1.0;
        } else {
            g *= std::sqrt(A);
            m0 = A * A;
            m1 = k * (1.0 - A) * A;
            m2 = 1.0 - A * A;
        }

        const double a1 = 1.0 / (1.0 + g * (g + k));
        const double a2 = g * a1;
        const double a3 = g * a2;
        m_a1 = static_cast<float>(a1);
        m_a2 = static_cast<float>(a2);
        m_a3 = static_cast<float>(a3);
        m_m0 = static_cast<float>(m0);
        m_m1 = static_cast<float>(m1);
        m_m2 = static_cast<float>(m2);
    }

    float m_fs = 48000.0f;
    Mode  m_cachedMode = Mode::Bell;
    float m_cachedFc = -1.0f;
    float m_cachedQ = -1.0f;
    float m_cachedGainDb = 999.0f;
    float m_cachedSat = -1.0f;
    bool  m_bypassed = true;
    float m_sat = 0.0f;

    float m_a1 = 0.0f, m_a2 = 0.0f, m_a3 = 0.0f;
    float m_m0 = 1.0f, m_m1 = 0.0f, m_m2 = 0.0f;
    float m_ic1eq = 0.0f;
    float m_ic2eq = 0.0f;
};

} // namespace stoneridge::vocal
