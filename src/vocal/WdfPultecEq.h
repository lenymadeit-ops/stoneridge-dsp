#pragma once

// Compact WDF-style Pultec low shelf interaction plus a TPT state-variable filter.
// The low boost and low cut share one state, so simultaneous boost+cut is not
// the sum of the two solo curves.

#include <algorithm>
#include <cmath>

namespace stoneridge::vocal {

class WdfPultecEq {
public:
    void Prepare(double sampleRate) noexcept {
        m_fs = (sampleRate > 1000.0) ? sampleRate : 48000.0;
        Reset();
    }

    void Reset() noexcept {
        m_cap = 0.0f;
        m_ic1eq = 0.0f;
        m_ic2eq = 0.0f;
    }

    inline float ProcessLow(float x, float boostDb, float cutDb) noexcept {
        const float boost = std::pow(10.0f, std::clamp(boostDb, -8.0f, 8.0f) / 20.0f);
        const float cut = std::pow(10.0f, std::clamp(cutDb, -8.0f, 0.0f) / 20.0f);
        const float g = static_cast<float>(1.0 - std::exp(-2.0 * 3.14159265358979323846 * 82.0 / m_fs));
        const float target = x * boost * cut;
        m_cap += (target - m_cap) * g;
        // Interaction: positive boost also opens the cut branch a little.
        const float dip = (boostDb > 0.0f) ? (0.0045f * boostDb) : 0.0f;
        return x + (m_cap - x) * (0.85f - dip);
    }

    // 12 kHz bell, trapezoidal SVF. Returns the bell-mixed sample.
    inline float ProcessBell(float x, float gainDb) noexcept {
        if (std::fabs(gainDb) < 0.02f) return x;
        const float A = std::pow(10.0f, std::clamp(gainDb, -8.0f, 10.0f) / 40.0f);
        const float fc = 12000.0f;
        const float g = std::tan(3.14159265358979323846f * fc / static_cast<float>(m_fs));
        const float k = 1.0f / 0.7f;
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float v3 = x - m_ic2eq;
        const float v1 = a1 * m_ic1eq + a1 * g * v3;
        const float v2 = m_ic2eq + g * v1;
        m_ic1eq = 2.0f * v1 - m_ic1eq;
        m_ic2eq = 2.0f * v2 - m_ic2eq;
        const float bell = v1 * (A - 1.0f);
        return x + bell;
    }

private:
    double m_fs = 48000.0;
    float m_cap = 0.0f;
    float m_ic1eq = 0.0f;
    float m_ic2eq = 0.0f;
};

class ZdfTptFilter {
public:
    void Prepare(double sampleRate) noexcept {
        m_fs = (sampleRate > 1000.0) ? static_cast<float>(sampleRate) : 48000.0f;
        Reset();
    }

    void Reset() noexcept { m_ic1eq = m_ic2eq = 0.0f; }

    inline float ProcessBell(float x, float fc, float q, float gainDb) noexcept {
        if (std::fabs(gainDb) < 0.02f) return x;
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float g = std::tan(3.14159265358979323846f * std::clamp(fc, 40.0f, m_fs * 0.45f) / m_fs);
        const float k = 1.0f / std::max(q, 0.35f);
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float v3 = x - m_ic2eq;
        const float v1 = a1 * m_ic1eq + a1 * g * v3;
        const float v2 = m_ic2eq + g * v1;
        m_ic1eq = 2.0f * v1 - m_ic1eq;
        m_ic2eq = 2.0f * v2 - m_ic2eq;
        const float sat = v1 / (1.0f + 0.08f * v1 * v1);
        return x + sat * (A - 1.0f);
    }

private:
    float m_fs = 48000.0f;
    float m_ic1eq = 0.0f;
    float m_ic2eq = 0.0f;
};

} // namespace stoneridge::vocal
