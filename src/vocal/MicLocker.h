#pragma once

// Stoneridge Vocal Library - platform-free, header-only, zero-allocation.
// Phase 4: Flagship Microphone Locker & Exact Cardioid Spherical-Wave Proximity Effect.
//
// Models:
//   0 = BypassRaw     (bit-exact bypass when virtualDistanceCm == 15.0 cm)
//   1 = SonyC800G     (K67-style bright/air + 6AU6 Peltier-cooled tube glow)
//   2 = NeumannU87Ai  (Smooth linear authority + clean Class-A FET)
//   3 = Telefunken251 (CK12 open top + 6072A twin-triode 2nd-harmonic shimmer)
//   4 = NeumannU47    (M7 chest/presence + VF14M pentode asymmetric warmth)
//   5 = ShureSm7b     (Dynamic low-end control + mid presence + smooth HF transient rounding)

#include "vocal/MicLockerCurves.h"
#include "vocal/Oversampler.h"

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

    void Prepare(double sampleRate) noexcept {
        m_fs = (sampleRate > 1000.0) ? sampleRate : 48000.0;
        m_os.Prepare(2u);
        m_cachedModel = 999u;
        m_cachedDistCm = -999.0f;
        Reset();
    }

    void Reset() noexcept {
        m_os.Reset();
        for (auto& b : m_eq) b.Reset();
        m_proxZ1 = 0.0f;
        m_proxPrevX = 0.0f;
        m_dcZ1 = 0.0f;
        m_dcPrevX = 0.0f;
    }

    void ProcessBlock(float* buf, uint32_t numFrames,
                      uint32_t micModelIdx, float virtualDistanceCm = kNominalDistanceCm) noexcept {
        if (!buf || numFrames == 0) return;

        const float clampedDist = std::clamp(virtualDistanceCm, 5.0f, 35.0f);
        const bool proxActive = std::fabs(clampedDist - kNominalDistanceCm) >= 0.25f;
        const bool modelActive = (micModelIdx >= 1u && micModelIdx <= kMicModelCount);

        // Bit-exact bypass when Raw & nominal 15 cm
        if (!modelActive && !proxActive) {
            return;
        }

        // 1. Update Spherical-Wave Proximity Compensator H_prox(s, r_target) / H_prox(s, r_ref)
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

        // 2. Minimum-phase on-axis curve cascade (fitted <= 0.35 dB across 50 Hz - 16 kHz)
        if (micModelIdx != m_cachedModel) {
            m_cachedModel = micModelIdx;
            UpdateModelCurve(micModelIdx - 1u);
        }
        for (uint32_t i = 0; i < numFrames; ++i) {
            float x = buf[i];
            for (uint32_t k = 0; k < kMicEqSections; ++k) {
                x = m_eq[k].Process(x);
            }
            buf[i] = x;
        }

        // 3. 2x oversampled model-specific Head-Amp Curvature (tube / FET / transformer)
        uint32_t offset = 0;
        while (offset < numFrames) {
            const uint32_t chunk = std::min<uint32_t>(numFrames - offset, Oversampler::kMaxBlock);
            float* hi = m_os.Upsample(buf + offset, chunk);
            const uint32_t osN = chunk * 2u;

            for (uint32_t k = 0; k < osN; ++k) {
                float u = hi[k];
                if (micModelIdx == static_cast<uint32_t>(MicModel::SonyC800G)) {
                    // 6AU6 tube asymmetric grid glow (clean even-order air polish)
                    u = u + 0.045f * u * u - 0.015f * u * u * u;
                } else if (micModelIdx == static_cast<uint32_t>(MicModel::NeumannU87Ai)) {
                    // Clean Class-A FET head amp
                    u = u - 0.012f * u * u * u + 0.010f * u * u;
                } else if (micModelIdx == static_cast<uint32_t>(MicModel::Telefunken251)) {
                    // 6072A twin-triode warm 2nd harmonic sheen
                    u = u + 0.060f * u * u - 0.020f * u * u * u;
                } else if (micModelIdx == static_cast<uint32_t>(MicModel::NeumannU47)) {
                    // VF14M pentode triode-strapped lush thickness
                    const float b = u + 0.08f;
                    u = (std::tanh(b * 0.72f) - std::tanh(0.08f * 0.72f)) / 0.72f;
                } else if (micModelIdx == static_cast<uint32_t>(MicModel::ShureSm7b)) {
                    // Dynamic coil + transformer soft transient compression
                    u = std::tanh(u * 0.65f) / 0.65f;
                }
                // Strip tiny asymmetric DC offset
                const float dcOut = u - m_dcPrevX + 0.9996f * m_dcZ1;
                m_dcPrevX = u;
                m_dcZ1 = dcOut;
                hi[k] = dcOut;
            }
            m_os.Downsample(buf + offset, chunk);
            offset += chunk;
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
        // Exact 1st-order spherical-wave cardioid proximity ratio:
        // H(s) = (s + alpha*c/r_target) / (s + alpha*c/r_ref)
        constexpr double kSpeedOfSound = 343.0;
        constexpr double kCardioidAlpha = 0.14; // Scaled for calibrated 14mm vocal cardioid gradient
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
    uint32_t m_cachedModel = 999u;
    float m_cachedDistCm   = -999.0f;

    float m_pb0 = 1.0f, m_pb1 = 0.0f, m_pa1 = 0.0f;
    float m_proxZ1 = 0.0f, m_proxPrevX = 0.0f;
    float m_dcZ1 = 0.0f, m_dcPrevX = 0.0f;

    std::array<Sec, kMicEqSections> m_eq{};
    Oversampler m_os;
};

} // namespace stoneridge::vocal
