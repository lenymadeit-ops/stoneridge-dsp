#pragma once

// Macro vocal polish: loudness contour, even-harmonic chest, a short de-harsher,
// and a parallel micro-leveler. Amount 0 is bit-transparent after the crossfade.

#include "vocal/SimdKernels.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace stoneridge::vocal {

struct BeautyParams {
    float beautyAmount = 0.0f;
    float contourAmt = 1.0f;
    float harmonicSculptAmt = 1.0f;
    float deHarshAmt = 1.0f;
    float parallelGlueAmt = 0.15f;
    float shortTermLufs = -18.0f;
};

class PsychoacousticBeauty {
public:
    void Prepare(double sampleRate) noexcept {
        m_fs = static_cast<float>((sampleRate > 1000.0) ? sampleRate : 48000.0);
        m_lpChest = 1.0f - std::exp(-2.0f * 3.14159265f * 260.0f / m_fs);
        m_lpAir = 1.0f - std::exp(-2.0f * 3.14159265f * 12000.0f / m_fs);
        m_hpAir = 1.0f - std::exp(-2.0f * 3.14159265f * 6000.0f / m_fs);
        m_env = 1.0f - std::exp(-1.0f / (0.008f * m_fs));
        m_fade.Prepare(sampleRate);
        Reset();
    }

    void Reset() noexcept {
        m_chest = m_chestQ = 0.0f;
        m_air = m_airHp = 0.0f;
        m_z1 = m_z2 = 0.0f;
        m_glue = 1.0f;
        m_fade.Reset();
    }

    inline float Process(float x, const BeautyParams& p) noexcept {
        const bool on = p.beautyAmount > 0.001f;
        m_fade.SetEnabled(on);
        const float g = m_fade.Next();
        if (m_fade.IsAsleep()) return x;

        const float amt = std::clamp(p.beautyAmount, 0.0f, 1.0f);
        const float loud = std::clamp((-18.0f - p.shortTermLufs) / 18.0f, 0.0f, 1.0f);
        const float contour = loud * amt * std::clamp(p.contourAmt, 0.0f, 1.0f);

        m_chest += (x - m_chest) * m_lpChest;
        const float hq = x - m_chest;
        m_chestQ += (hq - m_chestQ) * m_lpChest;
        const float h = m_chestQ;
        const float even = (m_chest * m_chest - h * h);
        const float chestOut = x + even * 0.08f * amt * std::clamp(p.harmonicSculptAmt, 0.0f, 1.0f);

        m_airHp += (chestOut - m_airHp) * m_hpAir;
        const float airBand = chestOut - m_airHp;
        m_air += (airBand - m_air) * m_lpAir;
        const float sib = std::fabs(airBand);
        const float airGain = (sib > 0.12f) ? 0.0f : (0.04f * amt);
        float y = chestOut + m_air * airGain;

        const float psi = m_z1 * m_z1 - m_z2 * x;
        m_z2 = m_z1;
        m_z1 = x;
        if (psi > 0.02f && std::fabs(x) > 0.05f) {
            const float cut = std::clamp(0.15f * amt * p.deHarshAmt * psi, 0.0f, 0.35f);
            y *= (1.0f - cut);
        }

        const float absY = std::fabs(y);
        const float over = std::max(0.0f, absY - 0.35f);
        const float glueTarget = 1.0f / (1.0f + 3.0f * over);
        m_glue += (glueTarget - m_glue) * m_env;
        const float glue = 1.0f + (m_glue - 1.0f) * std::clamp(p.parallelGlueAmt, 0.0f, 0.25f) * amt;
        y *= glue;

        const float lowLift = 1.0f + 0.12f * contour;
        y = y + (m_chest * (lowLift - 1.0f));
        return std::clamp(x + (y - x) * g, -1.0f, 1.0f);
    }

private:
    float m_fs = 48000.0f;
    float m_lpChest = 0.1f;
    float m_lpAir = 0.2f;
    float m_hpAir = 0.2f;
    float m_env = 0.01f;
    float m_chest = 0.0f;
    float m_chestQ = 0.0f;
    float m_air = 0.0f;
    float m_airHp = 0.0f;
    float m_z1 = 0.0f;
    float m_z2 = 0.0f;
    float m_glue = 1.0f;
    StageCrossfadeSleeper m_fade;
};

} // namespace stoneridge::vocal
