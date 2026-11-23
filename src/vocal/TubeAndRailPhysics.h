#pragma once

// Shared B+ reservoir and a smooth triode grid-bias stage. Sample-rate poles
// are set in Prepare. No heap, no locks.

#include "vocal/Adaa2MolecularShaper.h"

#include <algorithm>
#include <cmath>

namespace stoneridge::vocal {

class PowerSupplyRail {
public:
    void Prepare(double sampleRate) noexcept {
        const float fs = static_cast<float>((sampleRate > 1000.0) ? sampleRate : 48000.0);
        m_sag = 1.0f - std::exp(-1.0f / (0.018f * fs));
        m_rec = 1.0f - std::exp(-1.0f / (0.085f * fs));
        Reset();
    }

    void Reset() noexcept { m_droop = 0.0f; }

    // current is 0..1 instantaneous draw. Returns B+ scale in about 0.965..1.
    inline float Process(float current) noexcept {
        const float c = std::clamp(current, 0.0f, 1.0f);
        const float coeff = (c > m_droop) ? m_sag : m_rec;
        m_droop += (c - m_droop) * coeff;
        return 1.0f - 0.035f * m_droop;
    }

    float Droop() const noexcept { return m_droop; }

private:
    float m_sag = 0.001f;
    float m_rec = 0.0002f;
    float m_droop = 0.0f;
};

class TriodeGridBiasStage {
public:
    void Prepare(double sampleRate) noexcept {
        const float fs = static_cast<float>((sampleRate > 1000.0) ? sampleRate : 48000.0);
        m_biasCoeff = 1.0f - std::exp(-1.0f / (0.030f * fs));
        m_shaper.Prepare(sampleRate);
        Reset();
    }

    void Reset() noexcept {
        m_bias = 0.0f;
        m_shaper.Reset();
    }

    inline float Process(float x, float bPlus) noexcept {
        const float over = std::max(0.0f, x - 0.15f);
        const float target = -over;
        const float coeff = (target < m_bias) ? (m_biasCoeff * 2.2f) : (m_biasCoeff * 0.45f);
        m_bias += (target - m_bias) * std::clamp(coeff, 0.0f, 0.5f);
        const float driven = (x + 0.65f * m_bias) * bPlus;
        return m_shaper.Process(driven);
    }

private:
    float m_bias = 0.0f;
    float m_biasCoeff = 0.001f;
    Adaa2MolecularShaper m_shaper;
};

} // namespace stoneridge::vocal
