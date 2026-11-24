#pragma once

// Stoneridge Vocal Library - platform-free, header-only, zero-allocation.
// Flagship Microphone Locker: Spherical-Wave Cardioid Proximity, 4-tap Headbasket
// Micro-Diffraction, Minimum-Phase On-Axis Curves, Koren Triode / ADAA2 Head-Amps,
// Symplectic BAOAB Capsule Membrane (kHz resonance + excursion squeeze-film damping),
// and 15 ms equal-power model switching crossfade.

#include "vocal/Adaa2MolecularShaper.h"
#include "vocal/MicLockerCurves.h"
#include "vocal/Oversampler.h"
#include "vocal/TubeAndRailPhysics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace stoneridge::vocal {

enum class MicModel : uint32_t {
    BypassRaw     = 0,
    SonyC800G     = 1,
    NeumannU87Ai  = 2,
    Telefunken251 = 3,
    NeumannU47    = 4,
    ShureSm7b     = 5
};

class MicLockerStage {
public:
    static constexpr float kNominalDistanceCm = 15.0f;

    void Prepare(double sampleRate, bool internalOversample = true) noexcept {
        m_fs = (sampleRate > 1000.0) ? sampleRate : 48000.0;
        m_osFs = internalOversample ? (m_fs * 2.0) : m_fs;
        m_useInternalOs = internalOversample;
        m_os.Prepare(internalOversample ? 2u : 1u);
        PrepareCommon();
    }

    // Split-rate setup: linear EQ/proximity/headbasket run at baseRate while nonlinear head-amp + BAOAB run at osRate.
    void PrepareSplit(double baseRate, double osRate) noexcept {
        m_fs = (baseRate > 1000.0) ? baseRate : 48000.0;
        m_osFs = (osRate > 1000.0) ? osRate : m_fs;
        m_useInternalOs = false;
        m_os.Prepare(1u);
        PrepareCommon();
    }

    void PrepareCommon() noexcept {
        m_triode.Prepare(m_osFs, 1.15f);
        m_molShaper.Prepare(m_osFs);

        const float fsF = static_cast<float>(m_fs);
        m_dcCoeff = 1.0f - std::exp(-2.0f * 3.14159265358979323846f * 7.5f / static_cast<float>(m_osFs));
        m_xfadeStep = 1.0f / std::max(1.0f, 0.015f * fsF);

        // Precompute fractional headbasket tap positions at fs (0.08, 0.12, 0.17, 0.22 ms)
        constexpr float kTapMs[4] = { 0.08f, 0.12f, 0.17f, 0.22f };
        for (int t = 0; t < 4; ++t) {
            const float dSamples = std::clamp(kTapMs[t] * 0.001f * fsF, 1.0f, static_cast<float>(kBasketDelayMax - 4));
            const uint32_t i0 = static_cast<uint32_t>(dSamples);
            m_basketDelayI[t] = i0;
            m_basketDelayFrac[t] = dSamples - static_cast<float>(i0);
        }

        m_cachedModel = 0u;
        m_prevModel = 0u;
        m_hasModel = false;
        m_xfadePhase = 1.0f;
        m_cachedDistCm = -999.0f;
        Reset();
    }

    void Reset() noexcept {
        m_os.Reset();
        m_triode.Reset();
        m_molShaper.Reset();
        for (auto& b : m_eq) b.Reset();
        for (auto& b : m_prevEq) b.Reset();
        m_basketBuf.fill(0.0f);
        m_basketIdx = 0u;
        m_proxZ1 = 0.0f;
        m_proxPrevX = 0.0f;
        m_dcZ1 = 0.0f;
        m_dcPrevX = 0.0f;
        m_memX = 0.0f;
        m_memV = 0.0f;
        m_hasModel = false;
        m_xfadePhase = 1.0f;
    }

    void ProcessBlock(float* buf, uint32_t numFrames,
                      uint32_t micModelIdx, float virtualDistanceCm = kNominalDistanceCm,
                      float bPlus = 1.0f) noexcept {
        if (!buf || numFrames == 0) return;

        const float clampedDist = std::clamp(virtualDistanceCm, 5.0f, 35.0f);
        const bool proxActive = std::fabs(clampedDist - kNominalDistanceCm) >= 0.25f;
        const uint32_t targetModel = (micModelIdx >= 1u && micModelIdx <= kMicModelCount) ? micModelIdx : 0u;

        // Handle 15 ms equal-power model switch crossfade after initial model selection
        if (!m_hasModel) {
            m_hasModel = true;
            m_cachedModel = targetModel;
            m_prevModel = targetModel;
            if (targetModel >= 1u) {
                UpdateModelCurve(targetModel - 1u);
            } else {
                for (auto& b : m_eq) b.Reset();
            }
            m_xfadePhase = 1.0f;
        } else if (targetModel != m_cachedModel) {
            m_prevModel = m_cachedModel;
            m_prevEq = m_eq;
            m_cachedModel = targetModel;
            if (targetModel >= 1u) {
                UpdateModelCurve(targetModel - 1u);
            } else {
                for (auto& b : m_eq) b.Reset();
            }
            m_xfadePhase = 0.0f;
        }

        const bool crossfading = (m_xfadePhase < 1.0f);
        const bool modelActive = (targetModel >= 1u) || crossfading;

        // Bit-exact bypass when Raw, crossfade settled, & nominal 15 cm
        if (!modelActive && !proxActive) {
            return;
        }

        // 1. Spherical-Wave Cardioid Proximity Filter H_prox(s, r_target) / H_prox(s, r_ref)
        if (proxActive) {
            if (std::fabs(clampedDist - m_cachedDistCm) >= 0.1f) {
                m_cachedDistCm = clampedDist;
                UpdateProximityFilter(clampedDist);
            }
            for (uint32_t i = 0; i < numFrames; ++i) {
                const float x = buf[i];
                const float y = m_pb0 * x + m_pb1 * m_proxPrevX - m_pa1 * m_proxZ1;
                m_proxPrevX = x;
                m_proxZ1 = y;
                buf[i] = y;
            }
        }

        if (!modelActive) {
            return;
        }

        // 2. Minimum-phase on-axis EQ + 4-tap headbasket diffraction + head-amp + BAOAB capsule membrane
        const uint32_t activeCapsModel = (targetModel >= 1u) ? targetModel : m_prevModel;
        const CapsuleParams cap = GetCapsuleParams(activeCapsModel);
        const float dtBase = 1.0f / static_cast<float>(m_fs);
        const float omega0 = 2.0f * 3.14159265358979323846f * cap.resFreqHz;
        const uint32_t subSteps = std::max<uint32_t>(1u, static_cast<uint32_t>(std::ceil((omega0 * dtBase) / 0.35f)));
        const float dtSub = dtBase / static_cast<float>(subSteps);
        const float omegaSq = omega0 * omega0;
        const float halfDt = 0.5f * dtSub;

        constexpr float kBasketGain[4] = { 0.008f, -0.006f, 0.004f, -0.003f };
        constexpr float kHalfPi = 1.5707963267948966f;

        if (m_useInternalOs) {
            // Run linear EQ + headbasket + BAOAB, and run nonlinear head-amp 2x oversampled
            for (uint32_t i = 0; i < numFrames; ++i) {
                const float dry = buf[i];
                float wet = dry;
                if (targetModel >= 1u) {
                    for (uint32_t k = 0; k < kMicEqSections; ++k) {
                        wet = m_eq[k].Process(wet);
                    }
                    wet = ApplyHeadbasket(wet, kBasketGain);
                }

                float prevOut = dry;
                if (m_xfadePhase < 1.0f) {
                    if (m_prevModel >= 1u) {
                        for (uint32_t k = 0; k < kMicEqSections; ++k) {
                            prevOut = m_prevEq[k].Process(prevOut);
                        }
                    }
                    m_xfadePhase = std::min(1.0f, m_xfadePhase + m_xfadeStep);
                    const float gNew = std::sin(kHalfPi * m_xfadePhase);
                    const float gOld = std::cos(kHalfPi * m_xfadePhase);
                    buf[i] = prevOut * gOld + wet * gNew;
                } else {
                    buf[i] = wet;
                }
            }

            if (targetModel >= 1u) {
                uint32_t offset = 0;
                while (offset < numFrames) {
                    const uint32_t chunk = std::min<uint32_t>(numFrames - offset, Oversampler::kMaxBlock);
                    float* hi = m_os.Upsample(buf + offset, chunk);
                    const uint32_t osN = chunk * 2u;
                    for (uint32_t k = 0; k < osN; ++k) {
                        hi[k] = ProcessHeadAmpSample(hi[k], targetModel, bPlus);
                    }
                    m_os.Downsample(buf + offset, chunk);
                    offset += chunk;
                }
            }
        } else {
            // Already inside unified ~192 kHz oversampling domain (Zero internal Oversampler overhead)
            for (uint32_t i = 0; i < numFrames; ++i) {
                const float dry = buf[i];
                float wet = dry;
                if (targetModel >= 1u) {
                    for (uint32_t k = 0; k < kMicEqSections; ++k) {
                        wet = m_eq[k].Process(wet);
                    }
                    wet = ApplyHeadbasket(wet, kBasketGain);
                    wet = ProcessHeadAmpSample(wet, targetModel, bPlus);
                }

                if (m_xfadePhase < 1.0f) {
                    float prevOut = dry;
                    if (m_prevModel >= 1u) {
                        for (uint32_t k = 0; k < kMicEqSections; ++k) {
                            prevOut = m_prevEq[k].Process(prevOut);
                        }
                    }
                    m_xfadePhase = std::min(1.0f, m_xfadePhase + m_xfadeStep);
                    const float gNew = std::sin(kHalfPi * m_xfadePhase);
                    const float gOld = std::cos(kHalfPi * m_xfadePhase);
                    buf[i] = prevOut * gOld + wet * gNew;
                } else {
                    buf[i] = wet;
                }
            }
        }

        // 3. Symplectic BAOAB Capsule Membrane ODE with excursion-driven squeeze-film damping
        if (targetModel >= 1u) {
            for (uint32_t i = 0; i < numFrames; ++i) {
                const float xDrive = std::clamp(buf[i], -1.1f, 1.1f);
                for (uint32_t s = 0; s < subSteps; ++s) {
                    // Normalized excursion relative to backplate gap d0 (m_memX itself scales displacement)
                    const float normDisp = std::min(0.85f, std::fabs(m_memX) * 6.5f);
                    const float oneMinus = 1.0f - normDisp;
                    const float gamma = cap.gamma0 / (oneMinus * oneMinus * oneMinus);
                    const float gdt = gamma * halfDt;
                    const float damp = (gdt < 0.25f)
                        ? (1.0f - gdt + 0.5f * gdt * gdt)
                        : (1.0f / (1.0f + gdt + 0.5f * gdt * gdt));
                    m_memV += (-omegaSq * m_memX + xDrive * omegaSq * 0.018f) * halfDt;
                    m_memX += m_memV * halfDt;
                    m_memV *= damp;
                    m_memX += m_memV * halfDt;
                    m_memV += (-omegaSq * m_memX + xDrive * omegaSq * 0.018f) * halfDt;
                }
                buf[i] += cap.mixGain * 0.6f * m_memX;
            }
        }
    }

    // Runs linear proximity, 8-section on-axis EQ, 4-tap headbasket diffraction & 15 ms model crossfade at base rate (m_fs).
    void ProcessLinearBlock(float* buf, uint32_t numFrames,
                            uint32_t micModelIdx, float virtualDistanceCm = kNominalDistanceCm) noexcept {
        if (!buf || numFrames == 0) return;

        const float clampedDist = std::clamp(virtualDistanceCm, 5.0f, 35.0f);
        const bool proxActive = std::fabs(clampedDist - kNominalDistanceCm) >= 0.25f;
        const uint32_t targetModel = (micModelIdx >= 1u && micModelIdx <= kMicModelCount) ? micModelIdx : 0u;

        if (!m_hasModel) {
            m_hasModel = true;
            m_cachedModel = targetModel;
            m_prevModel = targetModel;
            if (targetModel >= 1u) {
                UpdateModelCurve(targetModel - 1u);
            } else {
                for (auto& b : m_eq) b.Reset();
            }
            m_xfadePhase = 1.0f;
        } else if (targetModel != m_cachedModel) {
            m_prevModel = m_cachedModel;
            m_prevEq = m_eq;
            m_cachedModel = targetModel;
            if (targetModel >= 1u) {
                UpdateModelCurve(targetModel - 1u);
            } else {
                for (auto& b : m_eq) b.Reset();
            }
            m_xfadePhase = 0.0f;
        }

        const bool crossfading = (m_xfadePhase < 1.0f);
        const bool modelActive = (targetModel >= 1u) || crossfading;
        if (!modelActive && !proxActive) return;

        if (proxActive) {
            if (std::fabs(clampedDist - m_cachedDistCm) >= 0.1f) {
                m_cachedDistCm = clampedDist;
                UpdateProximityFilter(clampedDist);
            }
            for (uint32_t i = 0; i < numFrames; ++i) {
                const float x = buf[i];
                const float y = m_pb0 * x + m_pb1 * m_proxPrevX - m_pa1 * m_proxZ1;
                m_proxPrevX = x;
                m_proxZ1 = y;
                buf[i] = y;
            }
        }
        if (!modelActive) return;

        constexpr float kBasketGain[4] = { 0.008f, -0.006f, 0.004f, -0.003f };
        constexpr float kHalfPi = 1.5707963267948966f;
        for (uint32_t i = 0; i < numFrames; ++i) {
            const float dry = buf[i];
            float wet = dry;
            if (targetModel >= 1u) {
                for (uint32_t k = 0; k < kMicEqSections; ++k) {
                    wet = m_eq[k].Process(wet);
                }
                wet = ApplyHeadbasket(wet, kBasketGain);
            }
            if (m_xfadePhase < 1.0f) {
                float prevOut = dry;
                if (m_prevModel >= 1u) {
                    for (uint32_t k = 0; k < kMicEqSections; ++k) {
                        prevOut = m_prevEq[k].Process(prevOut);
                    }
                }
                m_xfadePhase = std::min(1.0f, m_xfadePhase + m_xfadeStep);
                const float gNew = std::sin(kHalfPi * m_xfadePhase);
                const float gOld = std::cos(kHalfPi * m_xfadePhase);
                buf[i] = prevOut * gOld + wet * gNew;
            } else {
                buf[i] = wet;
            }
        }
    }

    // Runs nonlinear head-amp + BAOAB capsule membrane at the oversampled rate (m_osFs).
    void ProcessNonlinearBlock(float* buf, uint32_t numFrames,
                               uint32_t micModelIdx, float bPlus = 1.0f) noexcept {
        if (!buf || numFrames == 0) return;
        const uint32_t targetModel = (micModelIdx >= 1u && micModelIdx <= kMicModelCount) ? micModelIdx : 0u;
        if (targetModel == 0u) return;

        const CapsuleParams cap = GetCapsuleParams(targetModel);
        const float dtBase = 1.0f / static_cast<float>(m_osFs);
        const float omega0 = 2.0f * 3.14159265358979323846f * cap.resFreqHz;
        const uint32_t subSteps = std::max<uint32_t>(1u, static_cast<uint32_t>(std::ceil((omega0 * dtBase) / 0.35f)));
        const float dtSub = dtBase / static_cast<float>(subSteps);
        const float omegaSq = omega0 * omega0;
        const float halfDt = 0.5f * dtSub;

        for (uint32_t i = 0; i < numFrames; ++i) {
            float s = ProcessHeadAmpSample(buf[i], targetModel, bPlus);
            const float xDrive = std::clamp(s, -1.1f, 1.1f);
            for (uint32_t st = 0; st < subSteps; ++st) {
                const float normDisp = std::min(0.85f, std::fabs(m_memX) * 6.5f);
                const float oneMinus = 1.0f - normDisp;
                const float gamma = cap.gamma0 / (oneMinus * oneMinus * oneMinus);
                const float gdt = gamma * halfDt;
                const float damp = (gdt < 0.25f)
                    ? (1.0f - gdt + 0.5f * gdt * gdt)
                    : (1.0f / (1.0f + gdt + 0.5f * gdt * gdt));
                m_memV += (-omegaSq * m_memX + xDrive * omegaSq * 0.018f) * halfDt;
                m_memX += m_memV * halfDt;
                m_memV *= damp;
                m_memX += m_memV * halfDt;
                m_memV += (-omegaSq * m_memX + xDrive * omegaSq * 0.018f) * halfDt;
            }
            buf[i] = s + cap.mixGain * 0.6f * m_memX;
        }
    }

private:
    static constexpr uint32_t kBasketDelayMax = 64u;

    struct CapsuleParams {
        float resFreqHz;
        float gamma0;
        float mixGain;
    };

    static inline CapsuleParams GetCapsuleParams(uint32_t model) noexcept {
        switch (static_cast<MicModel>(model)) {
        case MicModel::SonyC800G:     return { 9500.0f,  18000.0f, 0.25f };
        case MicModel::NeumannU87Ai:  return { 8500.0f,  16000.0f, 0.20f };
        case MicModel::Telefunken251: return { 11200.0f, 20000.0f, 0.22f };
        case MicModel::NeumannU47:    return { 7200.0f,  14000.0f, 0.28f };
        case MicModel::ShureSm7b:     return { 4800.0f,  11000.0f, 0.30f };
        default:                      return { 8500.0f,  16000.0f, 0.0f };
        }
    }

    inline float ApplyHeadbasket(float x, const float gain[4]) noexcept {
        m_basketBuf[m_basketIdx] = x;
        float tapSum = 0.0f;
        for (int t = 0; t < 4; ++t) {
            const uint32_t i0 = (m_basketIdx - m_basketDelayI[t]) & (kBasketDelayMax - 1u);
            const uint32_t i1 = (i0 - 1u) & (kBasketDelayMax - 1u);
            const float s0 = m_basketBuf[i0];
            const float s1 = m_basketBuf[i1];
            const float delayed = s0 + (s1 - s0) * m_basketDelayFrac[t];
            tapSum += gain[t] * delayed;
        }
        m_basketIdx = (m_basketIdx + 1u) & (kBasketDelayMax - 1u);
        return x + tapSum;
    }

    inline float ProcessHeadAmpSample(float u, uint32_t micModelIdx, float bPlus) noexcept {
        float y = u;
        if (micModelIdx == static_cast<uint32_t>(MicModel::SonyC800G)) {
            // 6AU6 pentode/triode glow + subtle molecular polish
            y = m_triode.Process(u, bPlus, 0.85f);
        } else if (micModelIdx == static_cast<uint32_t>(MicModel::NeumannU87Ai)) {
            // Clean Class-A FET head amp
            y = u - 0.012f * u * u * u + 0.008f * u * u;
            m_molShaper.TrackIdle(u);
        } else if (micModelIdx == static_cast<uint32_t>(MicModel::Telefunken251)) {
            // 6072A twin-triode open shimmer
            y = m_triode.Process(u, bPlus, 1.05f);
        } else if (micModelIdx == static_cast<uint32_t>(MicModel::NeumannU47)) {
            // VF14M pentode asymmetric lush warmth via Koren triode bias stage
            y = m_triode.Process(u, bPlus, 1.25f);
        } else if (micModelIdx == static_cast<uint32_t>(MicModel::ShureSm7b)) {
            // Dynamic coil + transformer ADAA2 transient cushion (linear at low sweep amplitudes)
            if (std::fabs(u) < 0.09f) {
                m_molShaper.TrackIdle(u * 0.85f);
                y = u;
            } else {
                constexpr float kSm7bScale = 1.0f / (0.85f * kAcetanilideTorque.smallSignalGain);
                const float mol = m_molShaper.Process(std::clamp(u * 0.85f, -1.2f, 1.2f));
                const float satMix = std::clamp((std::fabs(u) - 0.09f) * 3.5f, 0.0f, 1.0f);
                y = u + (mol * kSm7bScale - u) * satMix;
            }
        }

        // Sample-rate-invariant DC blocker (~7.5 Hz)
        const float dcOut = y - m_dcPrevX + (1.0f - m_dcCoeff) * m_dcZ1;
        m_dcPrevX = y;
        m_dcZ1 = dcOut;
        return dcOut;
    }

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
        void Configure(uint8_t kind, float fc, float q, float gDb, double fs) noexcept {
            if (std::fabs(gDb) < 0.01f) { b0 = 1.0f; b1 = b2 = a1 = a2 = 0.0f; return; }
            constexpr float kPi = 3.14159265358979323846f;
            const float A = std::pow(10.0f, gDb / 40.0f);
            const float w0 = 2.0f * kPi * std::clamp(fc, 20.0f, static_cast<float>(fs * 0.45)) / static_cast<float>(fs);
            const float cs = std::cos(w0), sn = std::sin(w0);
            const float alpha = sn / (2.0f * std::max(q, 0.35f));
            if (kind == 1u) {
                const float tsa = 2.0f * std::sqrt(A) * alpha;
                const float invA0 = 1.0f / ((A + 1.0f) + (A - 1.0f) * cs + tsa);
                b0 = (A * ((A + 1.0f) - (A - 1.0f) * cs + tsa)) * invA0;
                b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs)) * invA0;
                b2 = (A * ((A + 1.0f) - (A - 1.0f) * cs - tsa)) * invA0;
                a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cs)) * invA0;
                a2 = ((A + 1.0f) + (A - 1.0f) * cs - tsa) * invA0;
            } else if (kind == 2u) {
                const float tsa = 2.0f * std::sqrt(A) * alpha;
                const float invA0 = 1.0f / ((A + 1.0f) - (A - 1.0f) * cs + tsa);
                b0 = (A * ((A + 1.0f) + (A - 1.0f) * cs + tsa)) * invA0;
                b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs)) * invA0;
                b2 = (A * ((A + 1.0f) + (A - 1.0f) * cs - tsa)) * invA0;
                a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cs)) * invA0;
                a2 = ((A + 1.0f) - (A - 1.0f) * cs - tsa) * invA0;
            } else {
                const float invA0 = 1.0f / (1.0f + alpha / A);
                b0 = (1.0f + alpha * A) * invA0;
                b1 = (-2.0f * cs) * invA0;
                b2 = (1.0f - alpha * A) * invA0;
                a1 = (-2.0f * cs) * invA0;
                a2 = (1.0f - alpha / A) * invA0;
            }
        }
    };

    void UpdateModelCurve(uint32_t idx) noexcept {
        const auto& tbl = kMicLockerEqCurves[std::min(idx, kMicModelCount - 1u)];
        for (uint32_t k = 0; k < kMicEqSections; ++k) {
            m_eq[k].Configure(tbl[k].kind, tbl[k].freqHz, tbl[k].q, tbl[k].gainDb, m_fs);
        }
    }

    void UpdateProximityFilter(float targetDistCm) noexcept {
        constexpr double kSpeedOfSound = 343.0;
        constexpr double kCardioidAlpha = 0.14;
        const double rTgt = static_cast<double>(targetDistCm) * 0.01;
        const double rRef = static_cast<double>(kNominalDistanceCm) * 0.01;
        const double wz = kCardioidAlpha * kSpeedOfSound / rTgt;
        const double wp = kCardioidAlpha * kSpeedOfSound / rRef;
        const double k2 = 2.0 * m_fs;
        const double invDen = 1.0 / (k2 + wp);
        m_pb0 = static_cast<float>((k2 + wz) * invDen);
        m_pb1 = static_cast<float>((-k2 + wz) * invDen);
        m_pa1 = static_cast<float>((-k2 + wp) * invDen);
    }

    double m_fs = 48000.0;
    double m_osFs = 96000.0;
    bool   m_useInternalOs = true;
    bool   m_hasModel = false;
    uint32_t m_cachedModel = 0u;
    uint32_t m_prevModel = 0u;
    float m_xfadePhase = 1.0f;
    float m_xfadeStep = 1.0f / 720.0f;
    float m_cachedDistCm = -999.0f;

    float m_pb0 = 1.0f, m_pb1 = 0.0f, m_pa1 = 0.0f;
    float m_proxZ1 = 0.0f, m_proxPrevX = 0.0f;
    float m_dcCoeff = 0.001f;
    float m_dcZ1 = 0.0f, m_dcPrevX = 0.0f;

    std::array<Sec, kMicEqSections> m_eq{};
    std::array<Sec, kMicEqSections> m_prevEq{};
    std::array<float, kBasketDelayMax> m_basketBuf{};
    uint32_t m_basketIdx = 0u;
    uint32_t m_basketDelayI[4]{ 1u, 1u, 1u, 1u };
    float    m_basketDelayFrac[4]{};

    Oversampler m_os;
    TriodeGridBiasStage m_triode;
    Adaa2MolecularShaper m_molShaper;
    float m_memX = 0.0f;
    float m_memV = 0.0f;
};

} // namespace stoneridge::vocal
