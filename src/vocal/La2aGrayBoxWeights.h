#pragma once

#include <cmath>

// Continuous-time diagonal SSM residual for the LA-2A gray box.
// alpha_i = exp(lambda_i / fs) is computed in Prepare so the time constants
// stay put at 44.1, 48, and 96 kHz. Weights are the scipy seed; retrain with
// tools/lab/lab/train_la2a_graybox.py.

namespace stoneridge::vocal {

struct La2aGrayBoxWeights {
    float lambda[8];
    float beta[8];
    float gamma[8];
    float delta;
};

inline constexpr La2aGrayBoxWeights kLa2aGrayBoxWeights{
    { -18.0f, -32.0f, -55.0f, -90.0f, -140.0f, -220.0f, -360.0f, -640.0f },
    { 0.04f, 0.03f, -0.02f, 0.015f, -0.01f, 0.008f, -0.004f, 0.002f },
    { 0.08f, 0.05f, 0.04f, -0.03f, 0.02f, -0.015f, 0.01f, -0.005f },
    0.0f
};

class La2aGrayBoxResidual {
public:
    void Prepare(double sampleRate) noexcept {
        const float fs = static_cast<float>((sampleRate > 1000.0) ? sampleRate : 48000.0);
        for (int i = 0; i < 8; ++i) {
            const float lambda = kLa2aGrayBoxWeights.lambda[i];
            m_alpha[i] = std::exp(lambda / fs);
            m_beta[i] = (kLa2aGrayBoxWeights.beta[i] / lambda) * (m_alpha[i] - 1.0f);
        }
        Reset();
    }

    void Reset() noexcept {
        for (float& h : m_h) h = 0.0f;
    }

    inline float Process(float u) noexcept {
        float y = kLa2aGrayBoxWeights.delta * u;
        for (int i = 0; i < 8; ++i) {
            m_h[i] = m_alpha[i] * m_h[i] + m_beta[i] * u;
            y += kLa2aGrayBoxWeights.gamma[i] * m_h[i];
        }
        return y;
    }

private:
    float m_alpha[8]{};
    float m_beta[8]{};
    float m_h[8]{};
};

} // namespace stoneridge::vocal
