#pragma once

// Restoring-torque ADAA2 waveshaper fit to the acetanilide PES.
// V(theta) is even around 180 deg, so the audio nonlinearity is the odd torque
// f(x) = -dV/dtheta, not the potential itself. Hysteresis is a separate state.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace stoneridge::vocal {

struct MolecularTorqueCoeffs {
    float a1, a2, a3, a4;
    float omega;
    float hyst;
};

// Cosine-series derivative of the 24-point PES, scaled into [-1, 1] drive.
inline constexpr MolecularTorqueCoeffs kAcetanilideTorque{
    0.62f, -0.18f, 0.07f, -0.025f, 1.35f, 0.055f
};

class Adaa2MolecularShaper {
public:
    void Prepare(double sampleRate) noexcept {
        m_hystCoeff = 1.0f - std::exp(-1.0f / (0.004f * static_cast<float>(std::max(1000.0, sampleRate))));
        Reset();
    }

    void Reset() noexcept {
        m_x1 = 0.0f;
        m_hyst = 0.0f;
    }

    inline float Process(float x) noexcept {
        const float target = (x >= 0.0f) ? kAcetanilideTorque.hyst : -kAcetanilideTorque.hyst;
        m_hyst += (target - m_hyst) * m_hystCoeff;
        const float driven = std::clamp(x + 0.15f * m_hyst, -1.25f, 1.25f);
        const float y = Adaa2(m_x1, driven);
        m_x1 = driven;
        return y;
    }

    static inline float Torque(float x) noexcept {
        const float w = kAcetanilideTorque.omega;
        const float a1 = kAcetanilideTorque.a1;
        const float a2 = kAcetanilideTorque.a2;
        const float a3 = kAcetanilideTorque.a3;
        const float a4 = kAcetanilideTorque.a4;
        return a1 * std::sin(w * x) + a2 * std::sin(2.0f * w * x)
             + a3 * std::sin(3.0f * w * x) + a4 * std::sin(4.0f * w * x);
    }

private:
    static inline float F1(float x) noexcept {
        const float w = kAcetanilideTorque.omega;
        const float a1 = kAcetanilideTorque.a1;
        const float a2 = kAcetanilideTorque.a2;
        const float a3 = kAcetanilideTorque.a3;
        const float a4 = kAcetanilideTorque.a4;
        return -a1 * std::cos(w * x) / w
             - a2 * std::cos(2.0f * w * x) / (2.0f * w)
             - a3 * std::cos(3.0f * w * x) / (3.0f * w)
             - a4 * std::cos(4.0f * w * x) / (4.0f * w);
    }

    static inline float F2(float x) noexcept {
        const float w = kAcetanilideTorque.omega;
        const float a1 = kAcetanilideTorque.a1;
        const float a2 = kAcetanilideTorque.a2;
        const float a3 = kAcetanilideTorque.a3;
        const float a4 = kAcetanilideTorque.a4;
        const float w2 = w * w;
        return -a1 * std::sin(w * x) / w2
             - a2 * std::sin(2.0f * w * x) / (4.0f * w2)
             - a3 * std::sin(3.0f * w * x) / (9.0f * w2)
             - a4 * std::sin(4.0f * w * x) / (16.0f * w2);
    }

    static inline float Adaa2(float x0, float x1) noexcept {
        const float dx = x1 - x0;
        if (std::fabs(dx) < 1.0e-4f) {
            const float mid = 0.5f * (x0 + x1);
            return Torque(mid);
        }
        const float y = (2.0f / (dx * dx)) * (F2(x1) - F2(x0) - F1(x0) * dx);
        if ((BitCast(y) & 0x7F800000u) == 0x7F800000u) return Torque(x1);
        return std::clamp(y, -1.5f, 1.5f);
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
    float m_hyst = 0.0f;
    float m_hystCoeff = 0.01f;
};

inline uint32_t AnalogOversampleFactor(double sampleRate, bool ecoMode) noexcept {
    if (ecoMode) return (sampleRate >= 88000.0) ? 1u : 2u;
    if (sampleRate >= 176000.0) return 1u;
    if (sampleRate >= 88000.0) return 2u;
    return 4u;
}

} // namespace stoneridge::vocal
