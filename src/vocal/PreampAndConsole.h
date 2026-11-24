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
#include "vocal/SimdKernels.h"
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
        SyncFloatCore();
        UpdateEddyCoeff();
        Reset();
    }

    void SetCoreParams(const JaCoreParams& p) noexcept {
        m_p = p;
        SyncFloatCore();
        UpdateEddyCoeff();
    }

    void Reset() noexcept {
        m_M        = 0.0f;
        m_Hprev    = 0.0f;
        m_Mprev    = 0.0f;
        m_eddyLp   = 0.0f;
    }

    // Evaluates 1 sample at the oversampled rate via series primary-winding
    // impedance-coupled hysteresis (R_src + L_primary(dM/dH) differential loading).
    inline float ProcessOversampled(float x, float ironMix, float driveGain) noexcept {
        if (ironMix <= 0.001f) {
            return x;
        }
        const float dGain = std::max(0.25f, driveGain);
        const float invDGain = 1.0f / dGain;
        // Magnetizing core excitation scales ~ 1/f at low frequencies (LF B-H saturation > HF).
        m_eddyLp += (x - m_eddyLp) * m_eddyAlpha;
        const float hDrive = dGain * (0.35f * x + 1.35f * m_eddyLp);
        const float rawDelta = hDrive - m_Hprev;

        const float chi = (std::fabs(rawDelta) > 1.0e-7f) ? EvalDmDh(m_Hprev, m_M, rawDelta) : m_chi0;
        const float seriesLoad = 1.0f / (1.0f + 0.14f * (chi * m_invSafeChi0));
        const float dH = rawDelta * seriesLoad;
        const float hNext = m_Hprev + dH;

        if (std::fabs(dH) > 1.0e-7f) {
            if (m_osRate >= 170000.0) {
                // At >= 170 kHz (~5 us step), 1-step Euler reuses chi directly (0 extra EvalDmDh).
                m_M = ClampM(m_M + chi * dH);
            } else {
                // seriesLoad > 0 preserves sign(dH) == sign(rawDelta), so k1 equals chi.
                const float k1 = chi;
                const float hMid = m_Hprev + 0.5f * dH;
                const float mMid = ClampM(m_M + 0.5f * k1 * dH);
                const float k2 = EvalDmDh(hMid, mMid, dH);
                m_M = ClampM(m_M + k2 * dH);
            }
        }

        const float dM = m_M - m_Mprev;
        m_Hprev = hNext;
        m_Mprev = m_M;

        const float satDeficit = (hNext - (m_M * m_invSafeChi0)) * invDGain;
        const float fluxTerm = (dM * m_invSafeChi0) * invDGain;
        const float mix = std::clamp(ironMix, 0.0f, 1.0f);
        return x - mix * (0.28f * satDeficit - 0.18f * fluxTerm);
    }

private:
    void SyncFloatCore() noexcept {
        m_Ms = static_cast<float>(m_p.Ms);
        m_a = std::max(0.01f, static_cast<float>(m_p.a));
        m_invA = 1.0f / m_a;
        m_alpha = static_cast<float>(m_p.alpha);
        m_k = static_cast<float>(m_p.k);
        m_c = static_cast<float>(m_p.c);
        m_oneMinusC = 1.0f - m_c;
        m_mLim = m_Ms * 0.995f;
        m_chi0 = m_Ms / (3.0f * std::max(0.05f, m_a));
        m_invSafeChi0 = 1.0f / std::max(0.5f, m_chi0);
    }

    void UpdateEddyCoeff() noexcept {
        constexpr double kPi = 3.14159265358979323846;
        m_eddyAlpha = static_cast<float>(1.0 - std::exp(-2.0 * kPi * m_p.eddyHz / std::max(44100.0, m_osRate)));
    }

    inline float ClampM(float m) const noexcept {
        return std::clamp(m, -m_mLim, m_mLim);
    }

    static inline void EvalLangevinAndDerivFast(float x, float& outL, float& outDL) noexcept {
        const float ax = std::fabs(x);
        if (ax < 0.08f) {
            const float x2 = x * x;
            outL  = x * ((1.0f / 3.0f) - x2 * ((1.0f / 45.0f) - x2 * (2.0f / 945.0f)));
            outDL = (1.0f / 3.0f) - x2 * ((1.0f / 15.0f) - x2 * (2.0f / 189.0f));
            return;
        }
        const float invX = 1.0f / x;
        if (ax > 18.0f) {
            outL  = ((x > 0.0f) ? 1.0f : -1.0f) - invX;
            outDL = invX * invX;
            return;
        }
        const float e = FastExp2(-2.88539008f * ax);
        const float cthAbs = (1.0f + e) / std::max(1.0e-6f, 1.0f - e);
        const float cth = (x > 0.0f) ? cthAbs : -cthAbs;
        outL  = cth - invX;
        outDL = (invX * invX) - (cthAbs * cthAbs - 1.0f);
    }

    inline float EvalDmDh(float H, float M, float dH) const noexcept {
        const float He = H + m_alpha * M;
        const float x = He * m_invA;
        float lv = 0.0f;
        float dlv = 0.0f;
        EvalLangevinAndDerivFast(x, lv, dlv);
        const float Man  = m_Ms * lv;
        const float dMan = (m_Ms * m_invA) * dlv;
        const float delta = (dH >= 0.0f) ? 1.0f : -1.0f;

        const float diff = Man - M;
        const float deltaM = ((dH >= 0.0f && diff > 0.0f) || (dH < 0.0f && diff < 0.0f)) ? 1.0f : 0.0f;

        const float denom = m_k * delta - m_alpha * m_oneMinusC * deltaM * diff;
        const float slope = (m_oneMinusC * deltaM * diff + m_c * m_k * delta * dMan)
                          / ((std::fabs(denom) > 1.0e-4f) ? denom : ((denom >= 0.0f) ? 1.0e-4f : -1.0e-4f));
        return std::clamp(slope, 0.0f, 12.0f);
    }

    JaCoreParams m_p{};
    double m_osRate    = 192000.0;
    float  m_Ms = 1.2f, m_a = 0.35f, m_invA = 1.0f / 0.35f, m_alpha = 0.08f, m_k = 0.28f, m_c = 0.25f;
    float  m_oneMinusC = 0.75f, m_mLim = 1.194f, m_chi0 = 1.1428f, m_invSafeChi0 = 1.0f / 1.1428f;
    float  m_eddyAlpha = 0.08f;
    float  m_M         = 0.0f;
    float  m_Hprev     = 0.0f;
    float  m_Mprev     = 0.0f;
    float  m_eddyLp    = 0.0f;
};

// Legendary Preamp Processor (4x oversampled or unified-rate JA iron + discrete transistor / Koren triode + 15 ms crossfade).
class PreampStage {
public:
    void Prepare(double sampleRate, bool internalOversample = true) noexcept {
        m_fs = (sampleRate > 1000.0) ? sampleRate : 48000.0;
        m_useInternalOs = internalOversample;
        m_os.Prepare(internalOversample ? 4u : 1u);
        const double osRate = internalOversample ? (m_fs * 4.0) : m_fs;
        m_ja.Prepare(m_fs, internalOversample ? 4u : 1u);
        m_rail.Prepare(m_fs);
        m_v76Triode.Prepare(osRate, 1.45f);
        m_dcCoeff = 1.0f - std::exp(-2.0f * 3.14159265358979323846f * 7.5f / static_cast<float>(osRate));
        m_xfadeStep = 1.0f / std::max(1.0f, 0.015f * static_cast<float>(m_fs));
        m_cachedModel = 0u;
        m_prevModel = 0u;
        m_hasModel = false;
        m_xfadePhase = 1.0f;
        UpdateModelBiquads(static_cast<uint32_t>(PreampModel::BypassClean));
        Reset();
    }

    void Reset() noexcept {
        m_os.Reset();
        m_ja.Reset();
        m_rail.Reset();
        m_v76Triode.Reset();
        m_dcZ1 = 0.0f;
        m_dcPrevIn = 0.0f;
        m_toneBiquad.Reset();
        m_prevToneBiquad.Reset();
        m_hasModel = false;
        m_xfadePhase = 1.0f;
    }

    void ProcessBlock(float* buf, uint32_t numFrames,
                      uint32_t modelIdx, float driveDb, float ironAmount,
                      float externalBPlus = -1.0f) noexcept {
        if (!buf || numFrames == 0) return;

        const uint32_t targetModel = (modelIdx <= static_cast<uint32_t>(PreampModel::Ssl9000J)) ? modelIdx : 0u;

        if (!m_hasModel) {
            m_hasModel = true;
            m_cachedModel = targetModel;
            m_prevModel = targetModel;
            ApplyModelCore(targetModel);
            UpdateModelBiquads(targetModel);
            m_xfadePhase = 1.0f;
        } else if (targetModel != m_cachedModel) {
            m_prevModel = m_cachedModel;
            m_prevToneBiquad = m_toneBiquad;
            m_cachedModel = targetModel;
            ApplyModelCore(targetModel);
            UpdateModelBiquads(targetModel);
            m_xfadePhase = 0.0f;
        }

        const bool crossfading = (m_xfadePhase < 1.0f);
        if (targetModel == static_cast<uint32_t>(PreampModel::BypassClean) && !crossfading) {
            return;
        }

        const float clampedDriveDb = std::clamp(driveDb, 0.0f, 18.0f);
        const float driveLin = std::pow(10.0f, clampedDriveDb * 0.05f);
        const float invOutLin = 1.0f / std::max(1.0f, std::pow(driveLin, 0.85f));
        const float effIron = (targetModel == static_cast<uint32_t>(PreampModel::Ssl9000J)
                            || targetModel == static_cast<uint32_t>(PreampModel::BypassClean))
                            ? 0.0f : std::clamp(ironAmount, 0.0f, 1.0f);

        constexpr float kHalfPi = 1.5707963267948966f;

        if (targetModel != static_cast<uint32_t>(PreampModel::BypassClean)
            && targetModel != static_cast<uint32_t>(PreampModel::Ssl9000J)) {
            for (uint32_t i = 0; i < numFrames; ++i) {
                const float dry = buf[i];
                const float wet = m_toneBiquad.Process(dry);
                if (crossfading) {
                    const float prev = (m_prevModel != 0u && m_prevModel != static_cast<uint32_t>(PreampModel::Ssl9000J))
                                     ? m_prevToneBiquad.Process(dry) : dry;
                    m_xfadePhase = std::min(1.0f, m_xfadePhase + m_xfadeStep);
                    const float gNew = std::sin(kHalfPi * m_xfadePhase);
                    const float gOld = std::cos(kHalfPi * m_xfadePhase);
                    buf[i] = prev * gOld + wet * gNew;
                } else {
                    buf[i] = wet;
                }
            }
        } else if (crossfading) {
            for (uint32_t i = 0; i < numFrames; ++i) {
                const float dry = buf[i];
                const float prev = (m_prevModel != 0u && m_prevModel != static_cast<uint32_t>(PreampModel::Ssl9000J))
                                 ? m_prevToneBiquad.Process(dry) : dry;
                m_xfadePhase = std::min(1.0f, m_xfadePhase + m_xfadeStep);
                const float gNew = std::sin(kHalfPi * m_xfadePhase);
                const float gOld = std::cos(kHalfPi * m_xfadePhase);
                buf[i] = prev * gOld + dry * gNew;
            }
        }

        if (targetModel == static_cast<uint32_t>(PreampModel::BypassClean)) {
            return;
        }

        const float railBPlus = (externalBPlus >= 0.0f) ? externalBPlus : 1.0f;

        auto fastTanh = [](float v) noexcept -> float {
            const float x = std::clamp(v, -3.0f, 3.0f);
            const float x2 = x * x;
            return x * (27.0f + x2) / (27.0f + 9.0f * x2);
        };

        auto processSampleNonlinear = [&](float raw, float bPlusVal) noexcept -> float {
            float u = raw * driveLin;
            if (targetModel == static_cast<uint32_t>(PreampModel::Neve1073)) {
                // Single-ended Class-A 2N3055: warm 2nd harmonic + gentle odd compression
                u = fastTanh(u * 0.85f + 0.09f * u * u) / 0.85f;
            } else if (targetModel == static_cast<uint32_t>(PreampModel::Api312)) {
                // Symmetric 2520 discrete op-amp: punchy 3rd harmonic transient grip
                u = fastTanh(u * 0.78f) / 0.78f;
            } else if (targetModel == static_cast<uint32_t>(PreampModel::TelefunkenV76)) {
                // Dual EF804S/12AX7 Koren triode plate + grid-leak bias recovery
                u = m_v76Triode.Process(u, bPlusVal, 1.0f);
            } else if (targetModel == static_cast<uint32_t>(PreampModel::Ssl9000J)) {
                // Transformerless SuperAnalogue: linear below clip, ultra-clean < 0.005% THD
                u = u / (1.0f + 0.008f * u * u);
            }

            if (effIron > 0.005f) {
                u = m_ja.ProcessOversampled(u, effIron, 0.8f + 0.4f * driveLin);
            }

            const float y = u - m_dcPrevIn + (1.0f - m_dcCoeff) * m_dcZ1;
            m_dcPrevIn = u;
            m_dcZ1 = y;
            return y * invOutLin;
        };

        if (m_useInternalOs) {
            uint32_t offset = 0;
            while (offset < numFrames) {
                const uint32_t chunk = std::min<uint32_t>(numFrames - offset, Oversampler::kMaxBlock);
                float* hi = m_os.Upsample(buf + offset, chunk);
                const uint32_t osFrames = chunk * 4u;
                for (uint32_t k = 0; k < osFrames; ++k) {
                    hi[k] = processSampleNonlinear(hi[k], railBPlus);
                }
                m_os.Downsample(buf + offset, chunk);
                offset += chunk;
            }
        } else {
            for (uint32_t i = 0; i < numFrames; ++i) {
                buf[i] = processSampleNonlinear(buf[i], railBPlus);
            }
        }

        if (externalBPlus < 0.0f) {
            for (uint32_t i = 0; i < numFrames; ++i) {
                const float draw = std::min(1.0f, std::fabs(buf[i]) * 1.5f);
                buf[i] *= m_rail.Process(draw);
            }
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

    double m_fs            = 48000.0;
    bool   m_useInternalOs = true;
    bool   m_hasModel      = false;
    uint32_t m_cachedModel = 0u;
    uint32_t m_prevModel   = 0u;
    float m_xfadePhase     = 1.0f;
    float m_xfadeStep      = 1.0f / 720.0f;
    float m_dcCoeff        = 0.00025f;
    float m_dcZ1           = 0.0f;
    float m_dcPrevIn       = 0.0f;
    Oversampler m_os;
    PowerSupplyRail m_rail;
    TriodeGridBiasStage m_v76Triode;
    JilesAthertonTransformer m_ja;
    SimpleBiquad m_toneBiquad;
    SimpleBiquad m_prevToneBiquad;
};

// Circuit-Derived Console EQ Stage (WDF Passive EQP-1A & Cached Cytomic SVF Console Filters).
class ConsoleEqStage {
public:
    void Prepare(double sampleRate) noexcept {
        m_fs = (sampleRate > 1000.0) ? sampleRate : 48000.0;
        m_cachedModel = 0u;
        m_prevModel = 0u;
        m_hasModel = false;
        m_xfadePhase = 1.0f;
        m_xfadeStep = 1.0f / std::max(1.0f, 0.015f * static_cast<float>(m_fs));
        m_wdf.Prepare(m_fs);
        m_zdfLow.Prepare(m_fs);
        m_zdfLowDip.Prepare(m_fs);
        m_zdfMid.Prepare(m_fs);
        m_zdfAir.Prepare(m_fs);
        m_prevZdfLow.Prepare(m_fs);
        m_prevZdfLowDip.Prepare(m_fs);
        m_prevZdfMid.Prepare(m_fs);
        m_prevZdfAir.Prepare(m_fs);
        Reset();
    }

    void Reset() noexcept {
        m_wdf.Reset();
        m_zdfLow.Reset();
        m_zdfLowDip.Reset();
        m_zdfMid.Reset();
        m_zdfAir.Reset();
        m_prevZdfLow.Reset();
        m_prevZdfLowDip.Reset();
        m_prevZdfMid.Reset();
        m_prevZdfAir.Reset();
        m_hasModel = false;
        m_xfadePhase = 1.0f;
    }

    void ProcessBlock(float* buf, uint32_t numFrames,
                      uint32_t eqModel, float lowDb, float midDb, float airDb,
                      bool useWdf = false) noexcept {
        if (!buf || numFrames == 0) return;

        const uint32_t targetModel = (eqModel <= static_cast<uint32_t>(ConsoleEqModel::Api550A)) ? eqModel : 0u;
        if (!m_hasModel) {
            m_hasModel = true;
            m_cachedModel = targetModel;
            m_prevModel = targetModel;
            m_xfadePhase = 1.0f;
        } else if (targetModel != m_cachedModel) {
            m_prevModel = m_cachedModel;
            m_prevZdfLow = m_zdfLow;
            m_prevZdfLowDip = m_zdfLowDip;
            m_prevZdfMid = m_zdfMid;
            m_prevZdfAir = m_zdfAir;
            m_cachedModel = targetModel;
            m_xfadePhase = 0.0f;
        }

        const bool crossfading = (m_xfadePhase < 1.0f);
        if (targetModel == static_cast<uint32_t>(ConsoleEqModel::Bypass) && !crossfading) {
            return;
        }
        if (std::fabs(lowDb) < 0.02f && std::fabs(midDb) < 0.02f && std::fabs(airDb) < 0.02f && !crossfading) {
            return;
        }

        const float clampedLow = std::clamp(lowDb, -8.0f, 8.0f);
        const float clampedMid = std::clamp(midDb, -8.0f, 8.0f);
        const float clampedAir = std::clamp(airDb, -6.0f, 10.0f);
        constexpr float kHalfPi = 1.5707963267948966f;

        auto evalModel = [&](float x, uint32_t model, ZdfTptFilter& sLow, ZdfTptFilter& sDip,
                             ZdfTptFilter& sMid, ZdfTptFilter& sAir, bool allowWdf) noexcept -> float {
            if (model == static_cast<uint32_t>(ConsoleEqModel::Bypass)) return x;
            float y = x;
            if (model == static_cast<uint32_t>(ConsoleEqModel::PultecEqp1a)) {
                if (allowWdf) {
                    const float boost = std::max(0.0f, clampedLow);
                    const float cut = (clampedLow < 0.0f) ? clampedLow : (-0.38f * clampedLow);
                    y = m_wdf.ProcessLow(y, boost, cut);
                    y = m_wdf.ProcessBell(y, clampedAir);
                    y = sMid.ProcessBell(y, 3200.0f, 0.85f, clampedMid, 0.0f);
                } else {
                    y = sLow.ProcessLowShelf(y, 82.0f, 0.65f, clampedLow * 1.15f);
                    if (clampedLow > 0.02f) y = sDip.ProcessBell(y, 210.0f, 0.95f, -clampedLow * 0.48f);
                    y = sMid.ProcessBell(y, 3000.0f, 1.10f, clampedMid);
                    y = sAir.ProcessBell(y, 12000.0f, 0.65f, clampedAir);
                }
            } else if (model == static_cast<uint32_t>(ConsoleEqModel::Neve1073Eq)) {
                y = sLow.ProcessLowShelf(y, 110.0f, 0.707f, clampedLow);
                const float qMid = 0.9f + 0.14f * std::fabs(clampedMid);
                y = sMid.ProcessBell(y, 1600.0f, qMid, clampedMid, allowWdf ? 0.06f : 0.0f);
                y = sAir.ProcessHighShelf(y, 12000.0f, 0.707f, clampedAir);
            } else if (model == static_cast<uint32_t>(ConsoleEqModel::Ssl4000G)) {
                y = sLow.ProcessLowShelf(y, 100.0f, 0.75f, clampedLow);
                const float qMid = 1.0f + 0.18f * std::fabs(clampedMid);
                y = sMid.ProcessBell(y, 3500.0f, qMid, clampedMid, 0.0f);
                y = sAir.ProcessHighShelf(y, 10000.0f, 0.707f, clampedAir);
            } else { // API 550A Proportional-Q with inductive mid bloom
                y = sLow.ProcessLowShelf(y, 100.0f, 0.85f, clampedLow);
                const float qMid = 0.75f + 0.25f * std::fabs(clampedMid);
                y = sMid.ProcessBell(y, 2400.0f, qMid, clampedMid, allowWdf ? 0.06f : 0.0f);
                y = sAir.ProcessHighShelf(y, 12500.0f, 0.75f, clampedAir);
            }
            return y;
        };

        for (uint32_t i = 0; i < numFrames; ++i) {
            const float x = buf[i];
            const float wet = evalModel(x, targetModel, m_zdfLow, m_zdfLowDip, m_zdfMid, m_zdfAir, useWdf);
            if (m_xfadePhase < 1.0f) {
                const float prev = evalModel(x, m_prevModel, m_prevZdfLow, m_prevZdfLowDip, m_prevZdfMid, m_prevZdfAir, false);
                m_xfadePhase = std::min(1.0f, m_xfadePhase + m_xfadeStep);
                const float gNew = std::sin(kHalfPi * m_xfadePhase);
                const float gOld = std::cos(kHalfPi * m_xfadePhase);
                buf[i] = prev * gOld + wet * gNew;
            } else {
                buf[i] = wet;
            }
        }
    }

private:
    double m_fs = 48000.0;
    uint32_t m_cachedModel = 0u;
    uint32_t m_prevModel   = 0u;
    bool   m_hasModel      = false;
    float  m_xfadePhase    = 1.0f;
    float  m_xfadeStep     = 1.0f / 720.0f;

    WdfPultecEq  m_wdf;
    ZdfTptFilter m_zdfLow, m_zdfLowDip, m_zdfMid, m_zdfAir;
    ZdfTptFilter m_prevZdfLow, m_prevZdfLowDip, m_prevZdfMid, m_prevZdfAir;
};

} // namespace stoneridge::vocal
