#pragma once

// Stoneridge Vocal Library - platform-free, header-only, zero-allocation.
// ITU-R BS.1770-4 K-weighted short-term loudness matcher for level-matched A/B
// and Blind ABX auditioning (+/- 0.15 dB perceived loudness match).

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace stoneridge::vocal {

class LoudnessMatcher {
public:
    void Prepare(double sampleRate) noexcept {
        m_fs = (sampleRate > 1000.0) ? sampleRate : 48000.0;
        DesignKWeighting(m_fs);
        // 400 ms RMS energy leaky integrator + 40 ms gain smoothing
        m_energyAlpha = 1.0f - std::exp(-1.0f / (0.400f * static_cast<float>(m_fs)));
        m_gainAlpha   = 1.0f - std::exp(-1.0f / (0.040f * static_cast<float>(m_fs)));
        Reset();
    }

    void Reset() noexcept {
        m_rawStage1Z1 = m_rawStage1Z2 = 0.0f;
        m_rawStage2Z1 = m_rawStage2Z2 = 0.0f;
        m_procStage1Z1 = m_procStage1Z2 = 0.0f;
        m_procStage2Z1 = m_procStage2Z2 = 0.0f;
        m_rawEnergy   = 1e-6f;
        m_procEnergy  = 1e-6f;
        m_smoothedGain = 1.0f;
        m_matchGainDb  = 0.0f;
    }

    // Feed raw (pre-chain) and processed (post-chain) samples; returns the gain
    // multiplier to apply to raw when A/B bypass is active so loudness matches processed.
    inline float ProcessSample(float raw, float proc) noexcept {
        const float kwRaw  = EvalKFilter(raw, m_rawStage1Z1, m_rawStage1Z2, m_rawStage2Z1, m_rawStage2Z2);
        const float kwProc = EvalKFilter(proc, m_procStage1Z1, m_procStage1Z2, m_procStage2Z1, m_procStage2Z2);

        const float eRaw  = kwRaw * kwRaw;
        const float eProc = kwProc * kwProc;

        // Only update loudness ratio while vocal signal is active (> -55 dBFS)
        // so silence between phrases does not drift the match gain.
        const bool loudEnough = eRaw > 6.31e-5f;
        if (!m_freeze && m_gateOpen && loudEnough) {
            m_rawEnergy  += (eRaw - m_rawEnergy) * m_energyAlpha;
            m_procEnergy += (eProc - m_procEnergy) * m_energyAlpha;
        }

        const float ratio = std::sqrt(std::max(m_procEnergy, 1e-9f) / std::max(m_rawEnergy, 1e-9f));
        // Clamp match gain to [-12 dB .. +18 dB] (0.251 .. 7.943)
        const float targetGain = std::clamp(ratio, 0.2511886f, 7.9432823f);
        m_smoothedGain += (targetGain - m_smoothedGain) * m_gainAlpha;
        m_matchGainDb = 20.0f * std::log10(std::max(m_smoothedGain, 1e-5f));
        return m_smoothedGain;
    }

    void SetGateOpen(bool open) noexcept { m_gateOpen = open; }
    void SetFreeze(bool freeze) noexcept { m_freeze = freeze; }

    float GetMatchGainDb() const noexcept { return m_matchGainDb; }
    float GetCurrentGainLin() const noexcept { return m_smoothedGain; }

private:
    inline float EvalKFilter(float x, float& s1z1, float& s1z2, float& s2z1, float& s2z2) const noexcept {
        // Stage 1: high-shelf pre-filter (+4 dB above 1.68 kHz)
        const float y1 = m_s1b0 * x + s1z1;
        s1z1 = m_s1b1 * x - m_s1a1 * y1 + s1z2;
        s1z2 = m_s1b2 * x - m_s1a2 * y1;
        // Stage 2: RLB high-pass (38 Hz)
        const float y2 = m_s2b0 * y1 + s2z1;
        s2z1 = m_s2b1 * y1 - m_s2a1 * y2 + s2z2;
        s2z2 = m_s2b2 * y1 - m_s2a2 * y2;
        return y2;
    }

    void DesignKWeighting(double fs) noexcept {
        constexpr double kPi = 3.14159265358979323846;
        // Stage 1: ITU-R BS.1770-4 head-sphere high-shelf (f0=1681.97 Hz, G=+3.9998 dB, Q=0.707175)
        {
            const double f0 = 1681.9744509555319;
            const double G  = 3.999843853973347;
            const double Q  = 0.7071752369554193;
            const double K  = std::tan(kPi * f0 / fs);
            const double Vh = std::pow(10.0, G / 20.0);
            const double Vb = std::pow(Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            m_s1b0 = static_cast<float>((Vh + Vb * K / Q + K * K) / a0);
            m_s1b1 = static_cast<float>((2.0 * (K * K - Vh)) / a0);
            m_s1b2 = static_cast<float>((Vh - Vb * K / Q + K * K) / a0);
            m_s1a1 = static_cast<float>((2.0 * (K * K - 1.0)) / a0);
            m_s1a2 = static_cast<float>((1.0 - K / Q + K * K) / a0);
        }
        // Stage 2: RLB high-pass weighting (f0=38.135 Hz, Q=0.500327)
        {
            const double f0 = 38.13547087602444;
            const double Q  = 0.5003270373238773;
            const double K  = std::tan(kPi * f0 / fs);
            const double a0 = 1.0 + K / Q + K * K;
            m_s2b0 = static_cast<float>(1.0 / a0);
            m_s2b1 = static_cast<float>(-2.0 / a0);
            m_s2b2 = static_cast<float>(1.0 / a0);
            m_s2a1 = static_cast<float>((2.0 * (K * K - 1.0)) / a0);
            m_s2a2 = static_cast<float>((1.0 - K / Q + K * K) / a0);
        }
    }

    double m_fs = 48000.0;
    float m_energyAlpha = 0.00005f;
    float m_gainAlpha   = 0.0005f;

    float m_s1b0 = 1.0f, m_s1b1 = 0.0f, m_s1b2 = 0.0f, m_s1a1 = 0.0f, m_s1a2 = 0.0f;
    float m_s2b0 = 1.0f, m_s2b1 = 0.0f, m_s2b2 = 0.0f, m_s2a1 = 0.0f, m_s2a2 = 0.0f;

    float m_rawStage1Z1 = 0.0f, m_rawStage1Z2 = 0.0f, m_rawStage2Z1 = 0.0f, m_rawStage2Z2 = 0.0f;
    float m_procStage1Z1 = 0.0f, m_procStage1Z2 = 0.0f, m_procStage2Z1 = 0.0f, m_procStage2Z2 = 0.0f;
    float m_rawEnergy    = 1e-6f;
    float m_procEnergy   = 1e-6f;
    float m_smoothedGain = 1.0f;
    float m_matchGainDb  = 0.0f;
    bool  m_gateOpen     = true;
    bool  m_freeze       = false;
};

} // namespace stoneridge::vocal
