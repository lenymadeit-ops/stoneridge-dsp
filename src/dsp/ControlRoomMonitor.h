#pragma once

// Monitor-only Control-Room Headphone Matrix (runs strictly on the DAC render path;
// NEVER modifies FL Studio recorded ASIO input buffers).
// Zero heap allocations, zero locks, static pre-allocated circular buffers.
//
// Features:
//  1. Vocalist Comfort Chamber: 4x4 Paraunitary Hadamard FDN (prime delays 11.3, 16.7,
//     23.1, 31.7 ms) with gentle HF air damping for inspiring headphone vocal monitoring.
//  2. Anti-Bleed Dynamic Cue Carve: sidechains the singer's vocal envelope to a subtle
//     -1.8 dB @ 2.2 kHz dip on the DAW backing playback so pitch stays clear at lower SPL.
//  3. BS2B-Style Binaural Control-Room Crossfeed: ~300 us interaural delay + 700 Hz
//     head-shadow cross-mix so headphones image like 60-degree studio monitors.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace stoneridge {

struct ControlRoomParams {
    float directMonitorMix    = 0.0f;   // 0.0 .. 1.0
    float monitorReverbAmount = 0.0f;   // 0.0 .. 1.0 (Comfort Chamber wet mix on monitor vocal)
    bool  crossfeedEnabled    = false;  // BS2B-style 300 us speaker crossfeed
    bool  cueCarveEnabled     = false;  // -1.8 dB @ 2.2 kHz dynamic backing duck when singing
};

class ControlRoomMonitor {
public:
    void Prepare(double sampleRate) noexcept {
        m_fs = (sampleRate > 1000.0) ? sampleRate : 48000.0;
        const float fs = static_cast<float>(m_fs);

        // Prime-ish FDN tap lengths scaled to sample rate (max 192 kHz -> 6100 samples < 8192)
        constexpr float kMs[4] = { 11.3f, 16.7f, 23.1f, 31.7f };
        for (int k = 0; k < 4; ++k) {
            m_fdnDelayLen[k] = std::clamp<uint32_t>(
                static_cast<uint32_t>(kMs[k] * 0.001f * fs), 64u, kFdnLineMax - 1u);
        }

        // Crossfeed delay (~300 us) & 700 Hz head-shadow 1-pole LPF
        m_xfeedDelayLen = std::clamp<uint32_t>(static_cast<uint32_t>(0.000300f * fs), 2u, kXfeedMax - 1u);
        m_xfeedLpCoeff  = 1.0f - std::exp(-2.0f * kPi * 700.0f / fs);
        m_fdnDampCoeff  = 1.0f - std::exp(-2.0f * kPi * 4800.0f / fs);

        // Anti-bleed cue carve sidechain envelope & 2.2 kHz bell (Q=1.0)
        m_cueAtkCoeff = 1.0f - std::exp(-1.0f / (0.008f * fs));
        m_cueRelCoeff = 1.0f - std::exp(-1.0f / (0.140f * fs));
        DesignCueBell(-1.8f, fs);
        Reset();
    }

    void Reset() noexcept {
        for (auto& line : m_fdnLines) line.fill(0.0f);
        m_fdnWriteIdx.fill(0);
        m_fdnDampState.fill(0.0f);
        m_xfeedBufL.fill(0.0f);
        m_xfeedBufR.fill(0.0f);
        m_xfeedWriteIdx = 0;
        m_xfeedLpL = 0.0f;
        m_xfeedLpR = 0.0f;
        m_cueEnv = 0.0f;
        m_cueDepthSmooth = 0.0f;
        m_cueZ1L = m_cueZ2L = 0.0f;
        m_cueZ1R = m_cueZ2R = 0.0f;
    }

    // Process stereo monitor block: mixes DAW playback (dawL/R) + direct monitor vocal (micL/R)
    // into dstL/dstR. Safe for aliasing pointers (e.g. dawL == dstL).
    void ProcessBlock(const float* dawL, const float* dawR,
                      const float* micL, const float* micR,
                      float* dstL, float* dstR,
                      uint32_t numFrames,
                      const ControlRoomParams& params) noexcept {
        const float monMix   = std::clamp(params.directMonitorMix, 0.0f, 1.0f);
        const bool  hasMon   = monMix > 0.001f;
        const bool  hasVerb  = hasMon && (params.monitorReverbAmount > 0.001f);
        const bool  hasCarve = hasMon && params.cueCarveEnabled;
        const bool  hasXfeed = params.crossfeedEnabled;

        // Fast bypass if monitor mix is off and crossfeed is off
        if (!hasMon && !hasXfeed) {
            if (dstL != dawL) {
                for (uint32_t i = 0; i < numFrames; ++i) dstL[i] = dawL[i];
            }
            if (dstR != dawR) {
                for (uint32_t i = 0; i < numFrames; ++i) dstR[i] = dawR[i];
            }
            return;
        }

        const float verbWet = hasVerb ? std::clamp(params.monitorReverbAmount, 0.0f, 1.0f) * 0.42f : 0.0f;

        for (uint32_t i = 0; i < numFrames; ++i) {
            float dL = dawL[i];
            float dR = dawR[i];
            const float mL = micL ? micL[i] : 0.0f;
            const float mR = micR ? micR[i] : mL;

            // 1. Anti-Bleed Dynamic Cue Carve (-1.8 dB @ 2.2 kHz on backing track while singing)
            if (hasCarve) {
                const float vAbs = std::max(std::fabs(mL), std::fabs(mR));
                m_cueEnv += (vAbs - m_cueEnv) * ((vAbs > m_cueEnv) ? m_cueAtkCoeff : m_cueRelCoeff);
                const float targetDepth = (m_cueEnv > 0.015f) ? std::clamp((m_cueEnv - 0.015f) * 8.0f, 0.0f, 1.0f) : 0.0f;
                m_cueDepthSmooth += (targetDepth - m_cueDepthSmooth) * m_cueAtkCoeff;
                if (m_cueDepthSmooth > 0.005f) {
                    const float carvedL = EvalCueBell(dL, m_cueZ1L, m_cueZ2L);
                    const float carvedR = EvalCueBell(dR, m_cueZ1R, m_cueZ2R);
                    dL = dL + (carvedL - dL) * m_cueDepthSmooth;
                    dR = dR + (carvedR - dR) * m_cueDepthSmooth;
                }
            }

            // 2. Monitor-Only Vocalist Comfort Chamber (4x4 Paraunitary Hadamard FDN)
            float monOutL = mL;
            float monOutR = mR;
            if (hasVerb) {
                const float monoIn = 0.5f * (mL + mR);
                float x[4];
                for (int k = 0; k < 4; ++k) {
                    const uint32_t rIdx = (m_fdnWriteIdx[k] + kFdnLineMax - m_fdnDelayLen[k]) & (kFdnLineMax - 1u);
                    const float rawTap = m_fdnLines[k][rIdx];
                    // 1-pole HF damping inside feedback loop
                    m_fdnDampState[k] += (rawTap - m_fdnDampState[k]) * m_fdnDampCoeff;
                    x[k] = m_fdnDampState[k];
                }
                // Orthonormal 4x4 Hadamard scattering matrix (0.5 * H_4) with RT60 ~ 1.1s feedback
                constexpr float g = 0.72f * 0.5f;
                const float h0 = ( x[0] + x[1] + x[2] + x[3]) * g;
                const float h1 = ( x[0] - x[1] + x[2] - x[3]) * g;
                const float h2 = ( x[0] + x[1] - x[2] - x[3]) * g;
                const float h3 = ( x[0] - x[1] - x[2] + x[3]) * g;
                const float fb[4] = { h0, h1, h2, h3 };
                for (int k = 0; k < 4; ++k) {
                    m_fdnLines[k][m_fdnWriteIdx[k]] = monoIn + fb[k];
                    m_fdnWriteIdx[k] = (m_fdnWriteIdx[k] + 1u) & (kFdnLineMax - 1u);
                }
                const float wetL = 0.5f * (x[0] - x[1] + x[2] - x[3]);
                const float wetR = 0.5f * (x[0] + x[1] - x[2] - x[3]);
                monOutL += wetL * verbWet;
                monOutR += wetR * verbWet;
            }

            float mixL = dL + (hasMon ? monOutL * monMix : 0.0f);
            float mixR = dR + (hasMon ? monOutR * monMix : 0.0f);

            // 3. BS2B-Style Binaural Control-Room Speaker Crossfeed (300 us + 700 Hz head shadow)
            if (hasXfeed) {
                m_xfeedBufL[m_xfeedWriteIdx] = mixL;
                m_xfeedBufR[m_xfeedWriteIdx] = mixR;
                const uint32_t rIdx = (m_xfeedWriteIdx + kXfeedMax - m_xfeedDelayLen) & (kXfeedMax - 1u);
                const float delL = m_xfeedBufL[rIdx];
                const float delR = m_xfeedBufR[rIdx];
                m_xfeedWriteIdx = (m_xfeedWriteIdx + 1u) & (kXfeedMax - 1u);

                m_xfeedLpL += (delL - m_xfeedLpL) * m_xfeedLpCoeff;
                m_xfeedLpR += (delR - m_xfeedLpR) * m_xfeedLpCoeff;

                // -9.5 dB (~0.335) cross-bleed with mild direct compensation to keep center energy flat
                mixL = mixL * 0.86f + m_xfeedLpR * 0.28f;
                mixR = mixR * 0.86f + m_xfeedLpL * 0.28f;
            }

            dstL[i] = std::clamp(mixL, -1.0f, 1.0f);
            dstR[i] = std::clamp(mixR, -1.0f, 1.0f);
        }
    }

private:
    static constexpr float    kPi         = 3.14159265358979323846f;
    static constexpr uint32_t kFdnLineMax = 8192;
    static constexpr uint32_t kXfeedMax   = 256;

    void DesignCueBell(float gainDb, float fs) noexcept {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = 2.0f * kPi * std::min(2200.0f, fs * 0.45f) / fs;
        const float cs = std::cos(w0);
        const float alpha = std::sin(w0) / (2.0f * 1.0f); // Q = 1.0
        const float invA0 = 1.0f / (1.0f + alpha / A);
        m_cb0 = (1.0f + alpha * A) * invA0;
        m_cb1 = (-2.0f * cs) * invA0;
        m_cb2 = (1.0f - alpha * A) * invA0;
        m_ca1 = (-2.0f * cs) * invA0;
        m_ca2 = (1.0f - alpha / A) * invA0;
    }

    inline float EvalCueBell(float x, float& z1, float& z2) const noexcept {
        const float y = m_cb0 * x + z1;
        z1 = m_cb1 * x - m_ca1 * y + z2;
        z2 = m_cb2 * x - m_ca2 * y;
        return y;
    }

    double m_fs = 48000.0;

    std::array<std::array<float, kFdnLineMax>, 4> m_fdnLines{};
    std::array<uint32_t, 4> m_fdnDelayLen{ 542u, 801u, 1108u, 1521u };
    std::array<uint32_t, 4> m_fdnWriteIdx{};
    std::array<float, 4>    m_fdnDampState{};
    float m_fdnDampCoeff = 0.45f;

    std::array<float, kXfeedMax> m_xfeedBufL{};
    std::array<float, kXfeedMax> m_xfeedBufR{};
    uint32_t m_xfeedDelayLen = 14u;
    uint32_t m_xfeedWriteIdx = 0;
    float m_xfeedLpCoeff = 0.08f;
    float m_xfeedLpL = 0.0f;
    float m_xfeedLpR = 0.0f;

    float m_cueAtkCoeff = 0.002f;
    float m_cueRelCoeff = 0.00015f;
    float m_cueEnv = 0.0f;
    float m_cueDepthSmooth = 0.0f;
    float m_cb0 = 1.0f, m_cb1 = 0.0f, m_cb2 = 0.0f, m_ca1 = 0.0f, m_ca2 = 0.0f;
    float m_cueZ1L = 0.0f, m_cueZ2L = 0.0f, m_cueZ1R = 0.0f, m_cueZ2R = 0.0f;
};

} // namespace stoneridge
