#pragma once

// Restoring-torque ADAA2 waveshaper fit to the acetanilide PES.
// V(theta) is even around 180 deg, so the audio nonlinearity is the odd torque
// f(x) = -dV/dtheta. True 3-sample triangular-kernel ADAA2 with smooth hysteresis.

#include "vocal/Adaa2MolecularCoeffs.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace stoneridge::vocal {

class Adaa2MolecularShaper {
public:
    void Prepare(double sampleRate) noexcept {
        const double sr = std::max(1000.0, sampleRate);
        m_velCoeff = 1.0f - std::exp(-1.0f / (0.002f * static_cast<float>(sr)));
        m_velScale = static_cast<float>(sr / 48000.0);
        Reset();
    }

    void Reset() noexcept {
        m_x1 = 0.0f;
        m_x2 = 0.0f;
        m_xPrevRaw = 0.0f;
        m_d1Prev = 0.0f;
        m_f2Prev = 0.0f;
        m_f2Prev2 = 0.0f;
        m_hasPrevD1 = false;
        m_hasF2Prev = false;
        m_velSmooth = 0.0f;
        m_envSmooth = 0.0f;
        m_hyst = 0.0f;
    }

    inline void TrackIdle(float x) noexcept {
        const float driven = std::clamp(x, -1.25f, 1.25f);
        m_xPrevRaw = x;
        m_x2 = m_x1;
        m_x1 = driven;
        m_hasPrevD1 = false;
        m_hasF2Prev = false;
        m_velSmooth *= (1.0f - m_velCoeff);
        m_envSmooth *= (1.0f - m_velCoeff);
        m_hyst = 0.0f;
    }

    inline float Process(float x) noexcept {
        const float velRaw = (x - m_xPrevRaw) * m_velScale;
        m_xPrevRaw = x;
        // Linear ~80 Hz 1-pole smoothing BEFORE FastTanh prevents high-frequency alias foldback.
        m_velSmooth += (velRaw - m_velSmooth) * m_velCoeff;
        m_envSmooth += (x * x - m_envSmooth) * m_velCoeff;
        const float ampScale = m_envSmooth / (0.04f + m_envSmooth);
        m_hyst = FastTanh(m_velSmooth * 18.0f) * ampScale * kAcetanilideTorque.hyst;

        const float driven = std::clamp(x + 0.15f * m_hyst, -1.25f, 1.25f);
        if (!m_hasF2Prev) {
            m_f2Prev    = F2(m_x1);
            m_f2Prev2   = F2(m_x2);
            m_hasF2Prev = true;
        }

        const float f2Curr = F2(driven);
        const float d1Curr = EvalD1Cached(driven, m_x1, f2Curr, m_f2Prev);
        if (!m_hasPrevD1) {
            m_d1Prev = EvalD1Cached(m_x1, m_x2, m_f2Prev, m_f2Prev2);
            m_hasPrevD1 = true;
        }

        const float span = driven - m_x2;
        float y;
        if (std::fabs(span) >= 1.5e-3f) {
            y = (2.0f / span) * (d1Curr - m_d1Prev);
        } else {
            const float xBar = 0.5f * (driven + m_x2);
            const float delta = m_x1 - xBar;
            if (std::fabs(delta) >= 1.5e-3f) {
                y = (2.0f / (delta * delta)) * ((m_f2Prev - F2(xBar)) - F1(xBar) * delta);
            } else {
                y = TorqueF((driven + m_x1 + m_x2) * (1.0f / 3.0f));
            }
        }

        m_x2 = m_x1;
        m_x1 = driven;
        m_f2Prev2 = m_f2Prev;
        m_f2Prev = f2Curr;
        m_d1Prev = d1Curr;

        if ((BitCast(y) & 0x7F800000u) == 0x7F800000u) {
            return std::clamp(Torque(driven), -1.0f, 1.0f);
        }
        return std::clamp(y, -1.0f, 1.0f);
    }

    static inline float Torque(float x) noexcept {
        return TorqueF(x);
    }

private:
    static inline float FastTanh(float v) noexcept {
        const float x = std::clamp(v, -3.0f, 3.0f);
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    static inline void FastSinCos(float u, float& s, float& c) noexcept {
        if (std::fabs(u) > 2.3f) {
            s = std::sin(u);
            c = std::cos(u);
            return;
        }
        const float u2 = u * u;
        s = u * (1.0f - u2 * ((1.0f / 6.0f) - u2 * ((1.0f / 120.0f) - u2 * ((1.0f / 5040.0f) - u2 / 362880.0f))));
        c = 1.0f - u2 * (0.5f - u2 * ((1.0f / 24.0f) - u2 * ((1.0f / 720.0f) - u2 / 40320.0f)));
    }

    static inline float TorqueF(float x) noexcept {
        const float u1 = kAcetanilideTorque.omega * x;
        float s1, c1;
        FastSinCos(u1, s1, c1);
        const float s2 = 2.0f * s1 * c1;
        const float c2 = 2.0f * c1 * c1 - 1.0f;
        const float s3 = s2 * c1 + c2 * s1;
        const float s4 = 2.0f * s2 * c2;
        return kAcetanilideTorque.a1 * s1
             + kAcetanilideTorque.a2 * s2
             + kAcetanilideTorque.a3 * s3
             + kAcetanilideTorque.a4 * s4;
    }

    // F1(x) = int_0^x f(t) dt, using 1 - cos(u) = 2*sin^2(u/2) via one half-angle FastSinCos.
    static inline float F1(float x) noexcept {
        const float w = kAcetanilideTorque.omega;
        float s1, c1;
        FastSinCos(0.5f * w * x, s1, c1);
        const float s2 = 2.0f * s1 * c1;
        const float c2 = 2.0f * c1 * c1 - 1.0f;
        const float s3 = s2 * c1 + c2 * s1;
        const float s4 = 2.0f * s2 * c2;
        const float invW = 2.0f / w;
        return invW * (
            kAcetanilideTorque.a1 * (s1 * s1)
          + (kAcetanilideTorque.a2 * 0.5f) * (s2 * s2)
          + (kAcetanilideTorque.a3 * (1.0f / 3.0f)) * (s3 * s3)
          + (kAcetanilideTorque.a4 * 0.25f) * (s4 * s4));
    }

    // F2(x) = int_0^x F1(t) dt, using Taylor u - sin(u) = u^3/6 - u^5/120 for tiny |u|.
    static inline float F2(float x) noexcept {
        const float w = kAcetanilideTorque.omega;
        const float u1 = w * x;
        const float invW2 = 1.0f / (w * w);
        if (std::fabs(u1) < 8.0e-3f) {
            auto taylorDiff = [&](float a, float m) noexcept -> float {
                const float u = m * u1;
                const float u2 = u * u;
                return (a / (m * m)) * (u * u2 * ((1.0f / 6.0f) - u2 * ((1.0f / 120.0f) - u2 / 5040.0f)));
            };
            return invW2 * (
                taylorDiff(kAcetanilideTorque.a1, 1.0f)
              + taylorDiff(kAcetanilideTorque.a2, 2.0f)
              + taylorDiff(kAcetanilideTorque.a3, 3.0f)
              + taylorDiff(kAcetanilideTorque.a4, 4.0f));
        }
        float s1, c1;
        FastSinCos(u1, s1, c1);
        const float s2 = 2.0f * s1 * c1;
        const float c2 = 2.0f * c1 * c1 - 1.0f;
        const float s3 = s2 * c1 + c2 * s1;
        const float s4 = 2.0f * s2 * c2;
        return invW2 * (
            kAcetanilideTorque.a1 * (u1 - s1)
          + (kAcetanilideTorque.a2 * 0.25f) * (2.0f * u1 - s2)
          + (kAcetanilideTorque.a3 * (1.0f / 9.0f)) * (3.0f * u1 - s3)
          + (kAcetanilideTorque.a4 * 0.0625f) * (4.0f * u1 - s4));
    }

    static inline float EvalD1Cached(float a, float b, float f2A, float f2B) noexcept {
        const float d = a - b;
        if (std::fabs(d) >= 1.5e-3f) {
            return (f2A - f2B) / d;
        }
        return F1(0.5f * (a + b));
    }

    static inline uint32_t BitCast(float x) noexcept {
        uint32_t u = 0;
        static_assert(sizeof(float) == sizeof(uint32_t), "float width");
        const auto* p = reinterpret_cast<const unsigned char*>(&x);
        u = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
          | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        return u;
    }

    float m_x1 = 0.0f;
    float m_x2 = 0.0f;
    float m_xPrevRaw = 0.0f;
    float m_d1Prev = 0.0f;
    float m_f2Prev = 0.0f;
    float m_f2Prev2 = 0.0f;
    bool  m_hasPrevD1 = false;
    bool  m_hasF2Prev = false;
    float m_velSmooth = 0.0f;
    float m_envSmooth = 0.0f;
    float m_hyst = 0.0f;
    float m_velCoeff = 0.01f;
    float m_velScale = 1.0f;
};

inline uint32_t AnalogOversampleFactor(double sampleRate, bool ecoMode) noexcept {
    if (ecoMode) return 1u;
    if (sampleRate >= 176000.0) return 1u;
    if (sampleRate >= 88000.0) return 2u;
    return 4u;
}

} // namespace stoneridge::vocal
