#pragma once

// Stoneridge Vocal Library - platform-free, header-only, zero-allocation.
// Phase 3: Ferromagnetic Transformer Preamps & Circuit-Derived Console EQs.
//
// Components:
//   1. LangevinExact(x)         : coth(x) - 1/x with 3-term Taylor series for |x| < 1e-3 (err < 1e-7).
//   2. JilesAthertonTransformer : Time-differential Jiles-Atherton hysteresis with 4x oversampled
//                                 RK2 trapezoidal step, bounded M clamping, and eddy-current pole.
//   3. PreampStage              : 4 Legendary Preamp Topologies (Clean/Bypass, Neve 1073 Class-A,
//                                 API 312/512c 2520, Telefunken V76 Tube, SSL 9000J SuperAnalogue).
//   4. ConsoleEqStage           : 4 Circuit-Derived Console EQs (Bypass, Pultec EQP-1A Passive LCR
//                                 Network, Neve 1073 Inductor, SSL 4000G, API 550A Proportional-Q).

#include "vocal/Oversampler.h"
#include "vocal/TubeAndRailPhysics.h"
#include "vocal/WdfPultecEq.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace stoneridge::vocal {

enum class PreampModel : uint32_t {
    BypassClean   = 0,
    Neve1073      = 1,
    Api312        = 2,
    TelefunkenV76 = 3,
    Ssl9000J      = 4
};

enum class ConsoleEqModel : uint32_t {
    Bypass      = 0,
    PultecEqp1a = 1,
    Neve1073Eq  = 2,
    Ssl4000G    = 3,
    Api550A     = 4
};

// Exact Langevin function L(x) = coth(x) - 1/x.
// Taylor expansion around x=0 avoids 0/0 cancellation: L(x) = x/3 - x^3/45 + 2*x^5/945.
inline double LangevinExact(double x) noexcept {
    const double ax = std::fabs(x);
    if (ax < 1e-3) {
        const double x2 = x * x;
        return x * ((1.0 / 3.0) - x2 * ((1.0 / 45.0) - x2 * (2.0 / 945.0)));
    }
    if (ax > 40.0) {
        return (x > 0.0) ? (1.0 - 1.0 / x) : (-1.0 - 1.0 / x);
    }
    const double ex  = std::exp(x);
    const double enx = 1.0 / ex;
    const double cth = (ex + enx) / (ex - enx);
    return cth - 1.0 / x;
}

// Analytical derivative dL/dx = 1/x^2 - csch^2(x).
inline double LangevinDeriv(double x) noexcept {
    const double ax = std::fabs(x);
    if (ax < 1e-3) {
        const double x2 = x * x;
        return (1.0 / 3.0) - x2 * ((1.0 / 15.0) - x2 * (2.0 / 189.0));
    }
    if (ax > 40.0) {
        return 1.0 / (x * x);
    }
    const double sh = std::sinh(x);
    return (1.0 / (x * x)) - (1.0 / (sh * sh));
}

// Ferromagnetic core hysteresis via Holters-Zolzer / Chowdhury state-space formulation.
// Runs at 4x oversampled sample rate for alias-free harmonic generation and frequency-dependent
// low-end B-H loop saturation (higher THD at 50 Hz than 1 kHz for realistic transformer iron).
struct JaCoreParams {
    double Ms    = 1.2;    // Saturation magnetization
    double a     = 0.35;   // Anhysteretic domain wall scaling
    double alpha = 0.08;   // Mean-field inter-domain coupling
    double k     = 0.28;   // Coercivity / pinning loss
    double c     = 0.25;   // Magnetization reversibility
    double eddyHz = 2600.0; // Low-frequency magnetic core emphasis corner
};

class JilesAthertonTransformer {
public:
    void Prepare(double baseSampleRate, uint32_t oversampleFactor = 4u) noexcept {
        m_osRate = ((baseSampleRate > 1000.0) ? baseSampleRate : 48000.0) * static_cast<double>(oversampleFactor);
        UpdateEddyCoeff();
        Reset();
    }

    void SetCoreParams(const JaCoreParams& p) noexcept {
        m_p = p;
        UpdateEddyCoeff();
    }

    void Reset() noexcept {
        m_M        = 0.0;
        m_Hprev    = 0.0;
        m_Mprev    = 0.0;
        m_eddyLp   = 0.0;
    }

    // Evaluates 1 sample at the oversampled rate (4x) using RK2 trapezoidal integration.
    // Returns blended hysteretic flux response (wet iron nonlinearity + clean direct balance).
    inline float ProcessOversampled(float x, float ironMix, float driveGain) noexcept {
        if (ironMix <= 0.001f) {
            return x;
        }
        // Frequency-dependent core excitation: transformer magnetizing current scales ~ 1/f
        // in low frequencies, modeled smoothly via a damped LF-emphasized magnetizing field.
        m_eddyLp += (static_cast<double>(x) - m_eddyLp) * m_eddyAlpha;
        const double hTarget = static_cast<double>(driveGain) * (0.35 * static_cast<double>(x) + 1.35 * m_eddyLp);
        const double dH = hTarget - m_Hprev;

        if (std::fabs(dH) > 1e-12) {
            const double k1 = EvalDmDh(m_Hprev, m_M, dH);
            const double hMid = m_Hprev + 0.5 * dH;
            const double mMid = ClampM(m_M + 0.5 * k1 * dH);
            const double k2 = EvalDmDh(hMid, mMid, dH);
            m_M = ClampM(m_M + k2 * dH);
        }

        const double fluxDeriv = (m_M - m_Mprev);
        m_Hprev = hTarget;
        m_Mprev = m_M;

        // Normalize linear small-signal gain near unity and blend with dry via ironMix
        const double normOut = (m_M / std::max(0.05, m_p.Ms)) * 0.55 + fluxDeriv * 1.4;
        const double mix = static_cast<double>(std::clamp(ironMix, 0.0f, 1.0f));
        const double blended = (1.0 - mix * 0.35) * static_cast<double>(x)
                             + (mix * 0.35) * (normOut / std::max(0.15, static_cast<double>(driveGain) * 0.52));
        return static_cast<float>(blended);
    }

private:
    void UpdateEddyCoeff() noexcept {
        constexpr double kPi = 3.14159265358979323846;
        m_eddyAlpha = 1.0 - std::exp(-2.0 * kPi * m_p.eddyHz / std::max(44100.0, m_osRate));
    }

    inline double ClampM(double m) const noexcept {
        const double lim = m_p.Ms * 0.995;
        return std::clamp(m, -lim, lim);
    }

    inline double EvalDmDh(double H, double M, double dH) const noexcept {
        const double He    = H + m_p.alpha * M;
        const double x     = He / std::max(0.01, m_p.a);
        const double Man   = m_p.Ms * LangevinExact(x);
        const double dMan  = (m_p.Ms / std::max(0.01, m_p.a)) * LangevinDeriv(x);
        const double delta = (dH >= 0.0) ? 1.0 : -1.0;

        // Prevent unphysical negative susceptibility when moving opposite to Man - M
        const double diff = Man - M;
        const double deltaM = ((dH >= 0.0 && diff > 0.0) || (dH < 0.0 && diff < 0.0)) ? 1.0 : 0.0;

        const double denom = m_p.k * delta - m_p.alpha * (1.0 - m_p.c) * deltaM * diff;
        double slope = ((1.0 - m_p.c) * deltaM * diff + m_p.c * m_p.k * delta * dMan)
                     / ((std::fabs(denom) > 1e-4) ? denom : ((denom >= 0.0) ? 1e-4 : -1e-4));
        return std::clamp(slope, 0.0, 12.0);
    }

    JaCoreParams m_p{};
    double m_osRate    = 192000.0;
    double m_eddyAlpha = 0.08;
    double m_M         = 0.0;
    double m_Hprev     = 0.0;
    double m_Mprev     = 0.0;
    double m_eddyLp    = 0.0;
};

// Legendary Preamp Processor (4x oversampled JA iron + discrete transistor/tube stage + contour).
class PreampStage {
public:
    void Prepare(double sampleRate) noexcept {
        m_fs = (sampleRate > 1000.0) ? sampleRate : 48000.0;
        m_os.Prepare(4u);
        m_ja.Prepare(m_fs, 4u);
        m_rail.Prepare(m_fs);
        m_cachedModel = 999u;
        UpdateModelBiquads(static_cast<uint32_t>(PreampModel::BypassClean));
        Reset();
    }

    void Reset() noexcept {
        m_os.Reset();
        m_ja.Reset();
        m_rail.Reset();
        m_dcZ1 = 0.0f;
        m_dcPrevIn = 0.0f;
        m_toneBiquad.Reset();
    }

    void ProcessBlock(float* buf, uint32_t numFrames,
                      uint32_t modelIdx, float driveDb, float ironAmount) noexcept {
        if (!buf || numFrames == 0) return;

        // Bit-exact bypass when Clean/Bypass model is selected
        if (modelIdx == static_cast<uint32_t>(PreampModel::BypassClean)) {
            return;
        }

        if (modelIdx != m_cachedModel) {
            m_cachedModel = modelIdx;
            ApplyModelCore(modelIdx);
            UpdateModelBiquads(modelIdx);
        }

        const float clampedDriveDb = std::clamp(driveDb, 0.0f, 18.0f);
        const float driveLin = std::pow(10.0f, clampedDriveDb * 0.05f);
        const float invOutLin = 1.0f / std::max(1.0f, std::pow(driveLin, 0.85f));
        const float effIron = (modelIdx == static_cast<uint32_t>(PreampModel::Ssl9000J))
                            ? 0.0f : std::clamp(ironAmount, 0.0f, 1.0f);

        // Apply subtle model-specific frequency contour (Marinair head-bump / 2520 punch)
        if (modelIdx != static_cast<uint32_t>(PreampModel::BypassClean)
            && modelIdx != static_cast<uint32_t>(PreampModel::Ssl9000J)) {
            for (uint32_t i = 0; i < numFrames; ++i) {
                buf[i] = m_toneBiquad.Process(buf[i]);
            }
        }

        uint32_t offset = 0;
        while (offset < numFrames) {
            const uint32_t chunk = std::min<uint32_t>(numFrames - offset, Oversampler::kMaxBlock);
            float* hi = m_os.Upsample(buf + offset, chunk);
            const uint32_t osFrames = chunk * 4u;

            for (uint32_t k = 0; k < osFrames; ++k) {
                float u = hi[k] * driveLin;

                // Discrete active stage nonlinearity (4x oversampled -> ultra-low aliasing)
                if (modelIdx == static_cast<uint32_t>(PreampModel::Neve1073)) {
                    // Single-ended Class-A 2N3055: warm 2nd harmonic + gentle odd compression
                    u = std::tanh(u * 0.85f + 0.09f * u * u) / 0.85f;
                } else if (modelIdx == static_cast<uint32_t>(PreampModel::Api312)) {
                    // Symmetric 2520 discrete op-amp: punchy 3rd harmonic transient grip
                    u = std::tanh(u * 0.78f) / 0.78f;
                } else if (modelIdx == static_cast<uint32_t>(PreampModel::TelefunkenV76)) {
                    // Dual EF804S tube: asymmetrical triode grid conduction (even harmonic glow)
                    const float bias = u + 0.14f;
                    u = (std::tanh(bias * 0.95f) - std::tanh(0.14f * 0.95f)) / 0.95f;
                } else if (modelIdx == static_cast<uint32_t>(PreampModel::Ssl9000J)) {
                    // Transformerless SuperAnalogue: linear below clip, ultra-clean < 0.005% THD
                    u = u / (1.0f + 0.008f * u * u);
                }

                // Ferromagnetic JA transformer hysteresis stage
                if (effIron > 0.005f) {
                    u = m_ja.ProcessOversampled(u, effIron, 0.8f + 0.4f * driveLin);
                }

                // Subtle 8 Hz AC-coupling DC blocker strips asymmetric bias offset
                const float y = u - m_dcPrevIn + 0.9997f * m_dcZ1;
                m_dcPrevIn = u;
                m_dcZ1 = y;
                hi[k] = y * invOutLin;
            }

            m_os.Downsample(buf + offset, chunk);
            offset += chunk;
        }
        for (uint32_t i = 0; i < numFrames; ++i) {
            const float draw = std::min(1.0f, std::fabs(buf[i]) * 1.5f);
            buf[i] *= m_rail.Process(draw);
        }
    }

private:
    struct SimpleBiquad {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;
        void Reset() noexcept { z1 = z2 = 0.0f; }
        inline float Process(float x) noexcept {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void SetPeaking(float freqHz, float q, float gainDb, double fs) noexcept {
            constexpr float kPi = 3.14159265358979323846f;
            const float A = std::pow(10.0f, gainDb / 40.0f);
            const float w0 = 2.0f * kPi * std::clamp(freqHz, 20.0f, static_cast<float>(fs * 0.45)) / static_cast<float>(fs);
            const float cs = std::cos(w0);
            const float alpha = std::sin(w0) / (2.0f * q);
            const float invA0 = 1.0f / (1.0f + alpha / A);
            b0 = (1.0f + alpha * A) * invA0;
            b1 = (-2.0f * cs) * invA0;
            b2 = (1.0f - alpha * A) * invA0;
            a1 = (-2.0f * cs) * invA0;
            a2 = (1.0f - alpha / A) * invA0;
        }
    };

    void ApplyModelCore(uint32_t modelIdx) noexcept {
        JaCoreParams p{};
        if (modelIdx == static_cast<uint32_t>(PreampModel::Neve1073)) {
            // Marinair LO1166 + T1444: rich LF iron memory
            p = { 1.15, 0.30, 0.10, 0.34, 0.22, 1800.0 };
        } else if (modelIdx == static_cast<uint32_t>(PreampModel::Api312)) {
            // AP2503 nickel-steel core: tighter lows, punchy midrange
            p = { 1.40, 0.42, 0.06, 0.24, 0.30, 3400.0 };
        } else if (modelIdx == static_cast<uint32_t>(PreampModel::TelefunkenV76)) {
            // Haufe high-inductance tube transformer: silky HF + broad LF bloom
            p = { 1.05, 0.28, 0.11, 0.22, 0.26, 2200.0 };
        } else {
            p = { 1.80, 0.50, 0.04, 0.15, 0.35, 3000.0 };
        }
        m_ja.SetCoreParams(p);
    }

    void UpdateModelBiquads(uint32_t modelIdx) noexcept {
        m_toneBiquad.Reset();
        if (modelIdx == static_cast<uint32_t>(PreampModel::Neve1073)) {
            m_toneBiquad.SetPeaking(75.0f, 0.9f, 1.2f, m_fs); // Subtle transformer low bump
        } else if (modelIdx == static_cast<uint32_t>(PreampModel::Api312)) {
            m_toneBiquad.SetPeaking(1400.0f, 0.85f, 1.0f, m_fs); // 2520 forward bark
        } else if (modelIdx == static_cast<uint32_t>(PreampModel::TelefunkenV76)) {
            m_toneBiquad.SetPeaking(110.0f, 0.75f, 1.1f, m_fs);
        } else {
            m_toneBiquad = SimpleBiquad{};
        }
    }

    double m_fs          = 48000.0;
    uint32_t m_cachedModel = 999u;
    float m_dcZ1         = 0.0f;
    float m_dcPrevIn     = 0.0f;
    Oversampler m_os;
    PowerSupplyRail m_rail;
    JilesAthertonTransformer m_ja;
    SimpleBiquad m_toneBiquad;
};

// Circuit-Derived Console EQ Stage (Pultec EQP-1A Passive LCR, Neve 1073, SSL 4000G, API 550A).
class ConsoleEqStage {
public:
    void Prepare(double sampleRate) noexcept {
        m_fs = (sampleRate > 1000.0) ? sampleRate : 48000.0;
        m_cachedModel = 999u;
        m_cachedLow = m_cachedMid = m_cachedAir = -999.0f;
        m_wdf.Prepare(m_fs);
        m_zdf.Prepare(m_fs);
        Reset();
    }

    void Reset() noexcept {
        m_low1.Reset();
        m_low2.Reset();
        m_mid.Reset();
        m_air.Reset();
        m_wdf.Reset();
        m_zdf.Reset();
    }

    void ProcessBlock(float* buf, uint32_t numFrames,
                      uint32_t eqModel, float lowDb, float midDb, float airDb,
                      bool useWdf = false) noexcept {
        if (!buf || numFrames == 0 || eqModel == static_cast<uint32_t>(ConsoleEqModel::Bypass)) {
            return;
        }
        if (std::fabs(lowDb) < 0.02f && std::fabs(midDb) < 0.02f && std::fabs(airDb) < 0.02f) {
            return;
        }

        if (useWdf && eqModel == static_cast<uint32_t>(ConsoleEqModel::PultecEqp1a)) {
            const float boost = std::max(0.0f, lowDb);
            const float cut = std::min(0.0f, -0.45f * std::max(lowDb, 0.0f));
            for (uint32_t i = 0; i < numFrames; ++i) {
                float y = m_wdf.ProcessLow(buf[i], boost, cut);
                y = m_wdf.ProcessBell(y, airDb);
                if (std::fabs(midDb) > 0.02f) y = m_zdf.ProcessBell(y, 3200.0f, 0.8f, midDb);
                buf[i] = y;
            }
            return;
        }

        if (eqModel != m_cachedModel || std::fabs(lowDb - m_cachedLow) > 0.02f
            || std::fabs(midDb - m_cachedMid) > 0.02f || std::fabs(airDb - m_cachedAir) > 0.02f) {
            m_cachedModel = eqModel;
            m_cachedLow   = lowDb;
            m_cachedMid   = midDb;
            m_cachedAir   = airDb;
            UpdateSections(eqModel, std::clamp(lowDb, -8.0f, 8.0f),
                           std::clamp(midDb, -8.0f, 8.0f), std::clamp(airDb, -6.0f, 10.0f));
        }

        for (uint32_t i = 0; i < numFrames; ++i) {
            float x = m_low1.Process(buf[i]);
            x = m_low2.Process(x);
            x = m_mid.Process(x);
            buf[i] = m_air.Process(x);
        }
    }

private:
    struct Sec {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;
        void Reset() noexcept { z1 = z2 = 0.0f; }
        inline float Process(float x) noexcept {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void SetPeaking(float fc, float q, float gDb, double fs) noexcept {
            if (std::fabs(gDb) < 0.02f) { b0 = 1.0f; b1 = b2 = a1 = a2 = 0.0f; return; }
            constexpr float kPi = 3.14159265358979323846f;
            const float A = std::pow(10.0f, gDb / 40.0f);
            const float w0 = 2.0f * kPi * std::clamp(fc, 20.0f, static_cast<float>(fs * 0.45)) / static_cast<float>(fs);
            const float cs = std::cos(w0);
            const float alpha = std::sin(w0) / (2.0f * std::max(q, 0.35f));
            const float invA0 = 1.0f / (1.0f + alpha / A);
            b0 = (1.0f + alpha * A) * invA0; b1 = (-2.0f * cs) * invA0; b2 = (1.0f - alpha * A) * invA0;
            a1 = (-2.0f * cs) * invA0; a2 = (1.0f - alpha / A) * invA0;
        }
        void SetLowShelf(float fc, float q, float gDb, double fs) noexcept {
            if (std::fabs(gDb) < 0.02f) { b0 = 1.0f; b1 = b2 = a1 = a2 = 0.0f; return; }
            constexpr float kPi = 3.14159265358979323846f;
            const float A = std::pow(10.0f, gDb / 40.0f);
            const float w0 = 2.0f * kPi * std::clamp(fc, 20.0f, static_cast<float>(fs * 0.45)) / static_cast<float>(fs);
            const float cs = std::cos(w0), sn = std::sin(w0);
            const float alpha = sn / (2.0f * std::max(q, 0.35f));
            const float tsa = 2.0f * std::sqrt(A) * alpha;
            const float invA0 = 1.0f / ((A + 1.0f) + (A - 1.0f) * cs + tsa);
            b0 = (A * ((A + 1.0f) - (A - 1.0f) * cs + tsa)) * invA0;
            b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs)) * invA0;
            b2 = (A * ((A + 1.0f) - (A - 1.0f) * cs - tsa)) * invA0;
            a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cs)) * invA0;
            a2 = ((A + 1.0f) + (A - 1.0f) * cs - tsa) * invA0;
        }
        void SetHighShelf(float fc, float q, float gDb, double fs) noexcept {
            if (std::fabs(gDb) < 0.02f) { b0 = 1.0f; b1 = b2 = a1 = a2 = 0.0f; return; }
            constexpr float kPi = 3.14159265358979323846f;
            const float A = std::pow(10.0f, gDb / 40.0f);
            const float w0 = 2.0f * kPi * std::clamp(fc, 20.0f, static_cast<float>(fs * 0.45)) / static_cast<float>(fs);
            const float cs = std::cos(w0), sn = std::sin(w0);
            const float alpha = sn / (2.0f * std::max(q, 0.35f));
            const float tsa = 2.0f * std::sqrt(A) * alpha;
            const float invA0 = 1.0f / ((A + 1.0f) - (A - 1.0f) * cs + tsa);
            b0 = (A * ((A + 1.0f) + (A - 1.0f) * cs + tsa)) * invA0;
            b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs)) * invA0;
            b2 = (A * ((A + 1.0f) + (A - 1.0f) * cs - tsa)) * invA0;
            a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cs)) * invA0;
            a2 = ((A + 1.0f) - (A - 1.0f) * cs - tsa) * invA0;
        }
    };

    void UpdateSections(uint32_t eqModel, float lowDb, float midDb, float airDb) noexcept {
        if (eqModel == static_cast<uint32_t>(ConsoleEqModel::PultecEqp1a)) {
            // Authentic passive EQP-1A LC bridging interaction:
            // Boost pole sits lower (70 Hz) than Atten pole (165 Hz), so positive Low Weight
            // naturally creates the famous 60-80 Hz chest lift + 180-240 Hz mud-carve dip!
            m_low1.SetLowShelf(82.0f, 0.65f, lowDb * 1.15f, m_fs);
            m_low2.SetPeaking(210.0f, 0.95f, (lowDb > 0.0f) ? (-lowDb * 0.48f) : 0.0f, m_fs);
            m_mid.SetPeaking(3000.0f, 1.10f, midDb, m_fs);
            m_air.SetPeaking(12000.0f, 0.65f, airDb, m_fs); // Classic inductor air bell
        } else if (eqModel == static_cast<uint32_t>(ConsoleEqModel::Neve1073Eq)) {
            m_low1.SetLowShelf(110.0f, 0.707f, lowDb, m_fs);
            m_low2.SetPeaking(100.0f, 1.0f, 0.0f, m_fs);
            const float qMid = 0.9f + 0.14f * std::fabs(midDb);
            m_mid.SetPeaking(1600.0f, qMid, midDb, m_fs);
            m_air.SetHighShelf(12000.0f, 0.707f, airDb, m_fs);
        } else if (eqModel == static_cast<uint32_t>(ConsoleEqModel::Ssl4000G)) {
            m_low1.SetLowShelf(100.0f, 0.75f, lowDb, m_fs);
            m_low2.SetPeaking(100.0f, 1.0f, 0.0f, m_fs);
            const float qMid = 1.0f + 0.18f * std::fabs(midDb);
            m_mid.SetPeaking(3500.0f, qMid, midDb, m_fs);
            m_air.SetHighShelf(10000.0f, 0.707f, airDb, m_fs);
        } else { // API 550A Proportional-Q
            m_low1.SetLowShelf(100.0f, 0.85f, lowDb, m_fs);
            m_low2.SetPeaking(100.0f, 1.0f, 0.0f, m_fs);
            const float qMid = 0.75f + 0.25f * std::fabs(midDb);
            m_mid.SetPeaking(2400.0f, qMid, midDb, m_fs);
            m_air.SetHighShelf(12500.0f, 0.75f, airDb, m_fs);
        }
    }

    double m_fs = 48000.0;
    uint32_t m_cachedModel = 999u;
    float m_cachedLow = -999.0f, m_cachedMid = -999.0f, m_cachedAir = -999.0f;
    Sec m_low1, m_low2, m_mid, m_air;
    WdfPultecEq m_wdf;
    ZdfTptFilter m_zdf;
};

} // namespace stoneridge::vocal
