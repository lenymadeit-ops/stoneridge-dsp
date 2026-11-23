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
        return static_cast<float>(TorqueD(static_cast<double>(x)));
    }

private:
    // Cosine series is ~0.5 at the origin. Scale so a quiet vocal is not dropped 6 dB.
    static inline double Unity() noexcept {
        const double w = kAcetanilideTorque.omega;
        const double a1 = kAcetanilideTorque.a1;
        const double a2 = kAcetanilideTorque.a2;
        const double a3 = kAcetanilideTorque.a3;
        const double a4 = kAcetanilideTorque.a4;
        const double slope = w * (a1 + 2.0 * a2 + 3.0 * a3 + 4.0 * a4);
        return (std::fabs(slope) > 1.0e-6) ? (1.0 / slope) : 1.0;
    }

    static inline double TorqueD(double x) noexcept {
        const double w = kAcetanilideTorque.omega;
        const double a1 = kAcetanilideTorque.a1;
        const double a2 = kAcetanilideTorque.a2;
        const double a3 = kAcetanilideTorque.a3;
        const double a4 = kAcetanilideTorque.a4;
        const double y = a1 * std::sin(w * x) + a2 * std::sin(2.0 * w * x)
                       + a3 * std::sin(3.0 * w * x) + a4 * std::sin(4.0 * w * x);
        return y * Unity();
    }

    static inline double F1(double x) noexcept {
        const double w = kAcetanilideTorque.omega;
        const double a1 = kAcetanilideTorque.a1;
        const double a2 = kAcetanilideTorque.a2;
        const double a3 = kAcetanilideTorque.a3;
        const double a4 = kAcetanilideTorque.a4;
        const double y = -a1 * std::cos(w * x) / w
                       - a2 * std::cos(2.0 * w * x) / (2.0 * w)
                       - a3 * std::cos(3.0 * w * x) / (3.0 * w)
                       - a4 * std::cos(4.0 * w * x) / (4.0 * w);
        return y * Unity();
    }

    static inline double F2(double x) noexcept {
        const double w = kAcetanilideTorque.omega;
        const double a1 = kAcetanilideTorque.a1;
        const double a2 = kAcetanilideTorque.a2;
        const double a3 = kAcetanilideTorque.a3;
        const double a4 = kAcetanilideTorque.a4;
        const double w2 = w * w;
        const double y = -a1 * std::sin(w * x) / w2
                       - a2 * std::sin(2.0 * w * x) / (4.0 * w2)
                       - a3 * std::sin(3.0 * w * x) / (9.0 * w2)
                       - a4 * std::sin(4.0 * w * x) / (16.0 * w2);
        return y * Unity();
    }

    // Double difference: float32 ADAA2 cancels at a sine peak and spikes past full scale.
    static inline float Adaa2(float x0, float x1) noexcept {
        const double dx = static_cast<double>(x1) - static_cast<double>(x0);
        double y;
        if (std::fabs(dx) < 1.0e-4) {
            y = TorqueD(0.5 * (static_cast<double>(x0) + static_cast<double>(x1)));
        } else {
            y = (2.0 / (dx * dx)) * (F2(x1) - F2(x0) - F1(x0) * dx);
        }
        const float yf = static_cast<float>(y);
        if ((BitCast(yf) & 0x7F800000u) == 0x7F800000u) {
            return std::clamp(Torque(x1), -1.0f, 1.0f);
        }
        return std::clamp(yf, -1.0f, 1.0f);
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
