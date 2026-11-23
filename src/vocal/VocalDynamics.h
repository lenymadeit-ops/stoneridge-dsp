#pragma once

// Stoneridge Vocal Library - platform-free, header-only, zero-allocation.
// Serial Studio Vocal Dynamics:
//   1. Fet1176Stage     : 1176-style fast feedback FET peak catcher (40-800 us attack,
//                         program-dependent release, 2x oversampled R_DS(V_GS) warmth).
//   2. OptoLa2aT4bStage : Teletronix LA-2A T4B coupled dual-memory CdS photoresistor ODE.

#include "vocal/La2aCalibration.h"
#include "vocal/La2aGrayBoxWeights.h"
#include "vocal/Oversampler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace stoneridge::vocal {

class Fet1176Stage {
public:
    void Prepare(double sampleRate) noexcept {
        m_fs = (sampleRate > 1000.0) ? static_cast<float>(sampleRate) : 48000.0f;
        m_os.Prepare(2);
        // 80 us attack, 50 ms fast peak release, 350 ms sustained release
        m_atkCoeff     = 1.0f - std::exp(-1.0f / (0.000080f * m_fs));
        m_relFastCoeff = 1.0f - std::exp(-1.0f / (0.050f * m_fs));
        m_relSlowCoeff = 1.0f - std::exp(-1.0f / (0.350f * m_fs));
        m_progCoeff    = 1.0f - std::exp(-1.0f / (0.120f * m_fs));
        Reset();
    }

    void Reset() noexcept {
        m_os.Reset();
        m_env         = 0.0f;
        m_progMem     = 0.0f;
        m_gainSmooth  = 1.0f;
        m_lastGrDb    = 0.0f;
    }

    // Processes mono buffer in-place when amount > 0.01f; returns minimum gain reduction (<= 0 dB).
    float ProcessBlock(float* buf, uint32_t numFrames, float amount, uint32_t ratioMode = 0u) noexcept {
        if (!buf || numFrames == 0 || amount <= 0.01f) {
            m_lastGrDb = 0.0f;
            return 0.0f;
        }

        const float amt      = std::clamp(amount, 0.0f, 1.0f);
        const float threshDb = -6.0f - 20.0f * amt; // -6 dBFS .. -26 dBFS
        const float ratio    = (ratioMode >= 2u) ? 20.0f : ((ratioMode == 1u) ? 8.0f : 4.0f);
        const float slope    = 1.0f - 1.0f / ratio;
        constexpr float kKneeDb = 4.0f;

        float minGain = 1.0f;

        // Stage A: Feedback FET gain-reduction envelope
        for (uint32_t i = 0; i < numFrames; ++i) {
            const float x = buf[i];
            // Feedback detector reads post-gain estimate
            const float fbAbs = std::fabs(x * m_gainSmooth);
            m_progMem += (fbAbs - m_progMem) * m_progCoeff;

            const float relCoeff = m_relFastCoeff + (m_relSlowCoeff - m_relFastCoeff)
                                 * std::clamp(m_progMem * 4.0f, 0.0f, 1.0f);
            m_env += (fbAbs - m_env) * ((fbAbs > m_env) ? m_atkCoeff : relCoeff);

            const float envDb = 20.0f * std::log10(std::max(m_env, 1e-5f));
            const float over  = envDb - threshDb;
            float grDb = 0.0f;
            if (over > 0.5f * kKneeDb) {
                grDb = -over * slope;
            } else if (over > -0.5f * kKneeDb) {
                const float t = over + 0.5f * kKneeDb;
                grDb = -slope * (t * t) / (2.0f * kKneeDb);
            }

            const float targetGain = std::pow(10.0f, grDb * 0.05f);
            const float gCoeff = (targetGain < m_gainSmooth) ? m_atkCoeff : relCoeff;
            m_gainSmooth += (targetGain - m_gainSmooth) * gCoeff;
            buf[i] = x * m_gainSmooth;
            minGain = std::min(minGain, m_gainSmooth);
        }

        // Stage B: 2x oversampled subtle FET R_DS(V_GS) asymmetric harmonic sheen
        const float satDrive = 0.12f * amt;
        if (satDrive > 0.005f) {
            uint32_t offset = 0;
            while (offset < numFrames) {
                const uint32_t chunk = std::min<uint32_t>(numFrames - offset, Oversampler::kMaxBlock);
                float* hi = m_os.Upsample(buf + offset, chunk);
                for (uint32_t k = 0; k < chunk * 2u; ++k) {
                    const float u = hi[k];
                    // Mild 2nd-order drain-source asymmetry + soft tanh ceiling
                    hi[k] = u + satDrive * (0.35f * u * u - 0.18f * u * u * u);
                }
                m_os.Downsample(buf + offset, chunk);
                offset += chunk;
            }
        }

        m_lastGrDb = 20.0f * std::log10(std::max(minGain, 1e-5f));
        return m_lastGrDb;
    }

    float GetLastGrDb() const noexcept { return m_lastGrDb; }

private:
    float m_fs           = 48000.0f;
    float m_atkCoeff     = 0.23f;
    float m_relFastCoeff = 0.0004f;
    float m_relSlowCoeff = 0.00006f;
    float m_progCoeff    = 0.00017f;
    float m_env          = 0.0f;
    float m_progMem      = 0.0f;
    float m_gainSmooth   = 1.0f;
    float m_lastGrDb     = 0.0f;
    Oversampler m_os;
};

class OptoLa2aT4bStage {
public:
    void Prepare(double sampleRate, const La2aT4bConstants& c = kLa2aDefaultConstants) noexcept {
        m_fs = (sampleRate > 1000.0) ? static_cast<float>(sampleRate) : 48000.0f;
        m_const = c;
        m_atkCoeff     = 1.0f - std::exp(-1.0f / (std::max(0.5f, c.tauAtkMs) * 0.001f * m_fs));
        m_chargeCoeff  = 1.0f - std::exp(-1.0f / (std::max(5.0f, c.tauChargeMs) * 0.001f * m_fs));
        m_dtMs         = 1000.0f / m_fs;
        m_gray.Prepare(m_fs);
        Reset();
    }

    void Reset() noexcept {
        m_gFast      = 0.0f;
        m_gSlow      = 0.0f;
        m_gainSmooth = 1.0f;
        m_lastGrDb   = 0.0f;
        m_gray.Reset();
    }

    // Processes mono buffer in-place when amount > 0.01f; returns minimum gain reduction (<= 0 dB).
    // Also exposes the instantaneous gain trajectory if outGainTrace != nullptr (for Test V2).
    float ProcessBlock(float* buf, uint32_t numFrames, float amount, float* outGainTrace = nullptr) noexcept {
        if (!buf || numFrames == 0 || amount <= 0.01f) {
            m_lastGrDb = 0.0f;
            if (outGainTrace) {
                for (uint32_t i = 0; i < numFrames; ++i) outGainTrace[i] = 1.0f;
            }
            return 0.0f;
        }

        const float amt      = std::clamp(amount, 0.0f, 1.0f);
        const float threshDb = m_const.threshMinDb + (m_const.threshMaxDb - m_const.threshMinDb) * amt;
        const float slope    = 1.0f - 1.0f / std::max(1.1f, m_const.ratio);
        const float kneeDb   = std::max(0.5f, m_const.kneeDb);

        float minGain = 1.0f;
        for (uint32_t i = 0; i < numFrames; ++i) {
            const float x = buf[i];
            const float absX = std::fabs(x);
            const float inDb = 20.0f * std::log10(std::max(absX, 1e-5f));
            const float over = inDb - threshDb;

            float targetCellDrive = 0.0f;
            if (over > 0.5f * kneeDb) {
                targetCellDrive = over * slope;
            } else if (over > -0.5f * kneeDb) {
                const float t = over + 0.5f * kneeDb;
                targetCellDrive = slope * (t * t) / (2.0f * kneeDb);
            }

            // Coupled dual-memory CdS photoresistor ODE:
            //   1. Deep trap-charge memory gSlow charges with tauChargeMs and decays with tauRelSlowMs
            const float slowCoeff = (targetCellDrive > m_gSlow)
                                  ? m_chargeCoeff
                                  : (m_dtMs / (m_const.tauRelSlowMs + m_dtMs));
            m_gSlow += (targetCellDrive - m_gSlow) * slowCoeff;

            //   2. Effective release tau_eff(t) lengthens monotonically with accumulated light memory
            const float memFrac = m_gSlow / (4.0f + m_gSlow);
            const float tauEffMs = m_const.tauRelFastMs + (m_const.tauRelSlowMs - m_const.tauRelFastMs) * memFrac;
            const float fastRelCoeff = m_dtMs / (tauEffMs + m_dtMs);
            const float fastCoeff = (targetCellDrive > m_gFast) ? m_atkCoeff : fastRelCoeff;
            m_gFast += (targetCellDrive - m_gFast) * fastCoeff;

            const float gainLin = std::pow(10.0f, -m_gFast * 0.05f);
            m_gainSmooth = gainLin;
            const float gained = x * gainLin;
            buf[i] = gained + 0.04f * m_gray.Process(gained);
            if (outGainTrace) outGainTrace[i] = gainLin;
            minGain = std::min(minGain, gainLin);
        }

        m_lastGrDb = 20.0f * std::log10(std::max(minGain, 1e-5f));
        return m_lastGrDb;
    }

    float GetLastGrDb() const noexcept { return m_lastGrDb; }

private:
    La2aT4bConstants m_const{};
    float m_fs          = 48000.0f;
    float m_dtMs        = 1.0f / 48.0f;
    float m_atkCoeff    = 0.002f;
    float m_chargeCoeff = 0.0001f;
    float m_gFast       = 0.0f;
    float m_gSlow       = 0.0f;
    float m_gainSmooth  = 1.0f;
    float m_lastGrDb    = 0.0f;
    La2aGrayBoxResidual m_gray;
};

} // namespace stoneridge::vocal
