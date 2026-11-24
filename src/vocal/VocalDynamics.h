#pragma once

// Stoneridge Vocal Library - platform-free, header-only, zero-allocation.
// Serial Studio Vocal Dynamics:
//   1. Fet1176Stage     : 1176-style fast feedback FET peak catcher with Shichman-Hodges
//                         triode shunt nonlinearity (THD scales monotonically with GR) & British All-Buttons mode.
//   2. OptoLa2aT4bStage : Teletronix LA-2A T4B coupled dual-memory CdS ODE with R37 HF emphasis,
//                         GR-domain continuous-time 8-state gray-box residual, and HA-100X/12AX7A stage.

#include "vocal/La2aCalibration.h"
#include "vocal/La2aGrayBoxWeights.h"
#include "vocal/Oversampler.h"
#include "vocal/SimdKernels.h"
#include "vocal/TubeAndRailPhysics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace stoneridge::vocal {

class Fet1176Stage {
public:
    void Prepare(double sampleRate, bool internalOversample = true) noexcept {
        m_fs = (sampleRate > 1000.0) ? static_cast<float>(sampleRate) : 48000.0f;
        m_useInternalOs = internalOversample;
        m_os[0].Prepare(internalOversample ? 2u : 1u);
        m_os[1].Prepare(internalOversample ? 2u : 1u);

        // 80 us attack, 50 ms fast peak release, 350 ms sustained release
        m_atkCoeff     = 1.0f - std::exp(-1.0f / (0.000080f * m_fs));
        m_relFastCoeff = 1.0f - std::exp(-1.0f / (0.050f * m_fs));
        m_relSlowCoeff = 1.0f - std::exp(-1.0f / (0.350f * m_fs));
        m_progCoeff    = 1.0f - std::exp(-1.0f / (0.120f * m_fs));
        const float osRate = internalOversample ? (m_fs * 2.0f) : m_fs;
        m_dcCoeff      = 1.0f - std::exp(-2.0f * 3.14159265358979323846f * 8.0f / osRate);
        Reset();
    }

    void Reset() noexcept {
        m_os[0].Reset();
        m_os[1].Reset();
        m_env        = 0.0f;
        m_progMem    = 0.0f;
        m_gainSmooth = 1.0f;
        m_lastGrDb   = 0.0f;
        m_dcX1[0] = m_dcX1[1] = 0.0f;
        m_dcY1[0] = m_dcY1[1] = 0.0f;
    }

    // Processes mono buffer in-place when amount > 0.01f; returns minimum gain reduction (<= 0 dB).
    float ProcessBlock(float* buf, uint32_t numFrames, float amount, uint32_t ratioMode = 0u) noexcept {
        return ProcessStereoBlock(buf, nullptr, numFrames, amount, ratioMode);
    }

    // Processes mono or stereo-linked buffers in-place; returns minimum gain reduction (<= 0 dB).
    float ProcessStereoBlock(float* left, float* right, uint32_t numFrames, float amount, uint32_t ratioMode = 0u) noexcept {
        if (!left || numFrames == 0 || amount <= 0.01f) {
            m_lastGrDb = 0.0f;
            return 0.0f;
        }

        const float amt      = std::clamp(amount, 0.0f, 1.0f);
        const bool  british  = (ratioMode >= 2u);
        // British All-Buttons-In pulls feedback bias harder (~1.5 dB deeper bite & punchy knee)
        const float threshDb = (-6.0f - 20.0f * amt) - (british ? 1.5f : 0.0f);
        const float ratio    = british ? 16.0f : ((ratioMode == 1u) ? 8.0f : 4.0f);
        const float slope    = 1.0f - 1.0f / ratio;
        const float kneeDb   = british ? 2.5f : 4.0f;
        const float britGain = british ? 1.65f : 1.0f;

        constexpr float kDbPerLog2 = 6.02059991f;
        constexpr float kLog2Per20 = 0.166096405f;

        float minGain = 1.0f;
        float chunkGain[Oversampler::kMaxBlock];

        uint32_t offset = 0;
        while (offset < numFrames) {
            const uint32_t chunk = std::min<uint32_t>(numFrames - offset, Oversampler::kMaxBlock);

            // Stage A: Stereo-linked Feedback FET envelope
            for (uint32_t i = 0; i < chunk; ++i) {
                const uint32_t idx = offset + i;
                const float xL = left[idx];
                const float xR = right ? right[idx] : xL;
                const float detIn = right ? std::max(std::fabs(xL), std::fabs(xR)) : std::fabs(xL);
                const float fbAbs = detIn * m_gainSmooth;
                m_progMem += (fbAbs - m_progMem) * m_progCoeff;

                const float relCoeff = m_relFastCoeff + (m_relSlowCoeff - m_relFastCoeff)
                                     * std::clamp(m_progMem * 4.0f, 0.0f, 1.0f);
                m_env += (fbAbs - m_env) * ((fbAbs > m_env) ? m_atkCoeff : relCoeff);

                const float envDb = kDbPerLog2 * FastLog2(std::max(m_env, 1.0e-5f));
                const float over  = envDb - threshDb;
                float grDb = 0.0f;
                if (over > 0.5f * kneeDb) {
                    grDb = -over * slope;
                } else if (over > -0.5f * kneeDb) {
                    const float t = over + 0.5f * kneeDb;
                    grDb = -slope * (t * t) / (2.0f * kneeDb);
                }

                const float targetGain = FastExp2(grDb * kLog2Per20);
                const float gCoeff = (targetGain < m_gainSmooth) ? m_atkCoeff : relCoeff;
                m_gainSmooth += (targetGain - m_gainSmooth) * gCoeff;
                chunkGain[i] = m_gainSmooth;
                minGain = std::min(minGain, m_gainSmooth);
            }

            // Stage B: Shichman-Hodges JFET triode shunt attenuation coupled monotonically to instantaneous GR
            auto runShichmanChannel = [&](float* chBuf, uint32_t chIdx) noexcept {
                if (m_useInternalOs) {
                    float* hi = m_os[chIdx].Upsample(chBuf + offset, chunk);
                    for (uint32_t i = 0; i < chunk; ++i) {
                        const float g = chunkGain[i];
                        const float grDepth = std::clamp(1.0f - g, 0.0f, 0.92f);
                        // Effective Shichman-Hodges channel loading scales directly with GR gate bias modulation
                        const float idsBeta = grDepth * 0.42f * britGain;
                        for (uint32_t s = 0; s < 2u; ++s) {
                            const float u = hi[2u * i + s];
                            const float vds = u * g;
                            const float nonlin = vds / (1.0f + idsBeta * (0.28f * vds - 0.14f * vds * vds));
                            const float dcFree = nonlin - m_dcX1[chIdx] + (1.0f - m_dcCoeff) * m_dcY1[chIdx];
                            m_dcX1[chIdx] = nonlin;
                            m_dcY1[chIdx] = dcFree;
                            hi[2u * i + s] = dcFree;
                        }
                    }
                    m_os[chIdx].Downsample(chBuf + offset, chunk);
                } else {
                    for (uint32_t i = 0; i < chunk; ++i) {
                        const float g = chunkGain[i];
                        const float grDepth = std::clamp(1.0f - g, 0.0f, 0.92f);
                        const float idsBeta = grDepth * 0.42f * britGain;
                        const float vds = chBuf[offset + i] * g;
                        const float nonlin = vds / (1.0f + idsBeta * (0.28f * vds - 0.14f * vds * vds));
                        const float dcFree = nonlin - m_dcX1[chIdx] + (1.0f - m_dcCoeff) * m_dcY1[chIdx];
                        m_dcX1[chIdx] = nonlin;
                        m_dcY1[chIdx] = dcFree;
                        chBuf[offset + i] = dcFree;
                    }
                }
            };

            runShichmanChannel(left, 0u);
            if (right) runShichmanChannel(right, 1u);

            offset += chunk;
        }

        m_lastGrDb = kDbPerLog2 * FastLog2(std::max(minGain, 1.0e-5f));
        return m_lastGrDb;
    }

    float GetLastGrDb() const noexcept { return m_lastGrDb; }

private:
    float m_fs           = 48000.0f;
    bool  m_useInternalOs = true;
    float m_atkCoeff     = 0.23f;
    float m_relFastCoeff = 0.0004f;
    float m_relSlowCoeff = 0.00006f;
    float m_progCoeff    = 0.00017f;
    float m_dcCoeff      = 0.0005f;
    float m_env          = 0.0f;
    float m_progMem      = 0.0f;
    float m_gainSmooth   = 1.0f;
    float m_lastGrDb     = 0.0f;
    float m_dcX1[2]{};
    float m_dcY1[2]{};
    Oversampler m_os[2];
};

class OptoLa2aT4bStage {
public:
    void Prepare(double sampleRate, const La2aT4bConstants& c = kLa2aDefaultConstants) noexcept {
        m_fs = (sampleRate > 1000.0) ? static_cast<float>(sampleRate) : 48000.0f;
        m_const = c;
        m_atkCoeff     = 1.0f - std::exp(-1.0f / (std::max(0.5f, c.tauAtkMs) * 0.001f * m_fs));
        m_chargeCoeff  = 1.0f - std::exp(-1.0f / (std::max(5.0f, c.tauChargeMs) * 0.001f * m_fs));
        m_dtMs         = 1000.0f / m_fs;

        // Continuous-time bilinear 1st-order R37 HF emphasis shelf (~2.2 kHz)
        constexpr float kPi = 3.14159265358979323846f;
        const float w0 = 2.0f * kPi * kLa2aR37Constants.shelfFreqHz / m_fs;
        const float A = std::pow(10.0f, kLa2aR37Constants.shelfGainDb / 40.0f);
        const float g = std::tan(0.5f * w0);
        const float invA = 1.0f / A;
        const float denom = g + invA;
        m_r37B0 = (g + A) / denom;
        m_r37B1 = (g - A) / denom;
        m_r37A1 = (g - invA) / denom;

        m_grGray.Prepare(m_fs);
        m_grGrayHalf.Prepare((m_fs >= 170000.0f) ? (m_fs * 0.5f) : m_fs);
        m_r37HalfW0 = 2.0f * kPi * kLa2aR37Constants.shelfFreqHz / ((m_fs >= 170000.0f) ? (m_fs * 0.5f) : m_fs);
        {
            const float gh = std::tan(0.5f * m_r37HalfW0);
            const float dh = gh + invA;
            m_r37HalfB0 = (gh + A) / dh;
            m_r37HalfB1 = (gh - A) / dh;
            m_r37HalfA1 = (gh - invA) / dh;
        }
        m_audioStage[0].Prepare(m_fs);
        m_audioStage[1].Prepare(m_fs);
        m_triode[0].Prepare(m_fs, 0.95f);
        m_triode[1].Prepare(m_fs, 0.95f);
        Reset();
    }

    void Reset() noexcept {
        m_gFast      = 0.0f;
        m_gSlow      = 0.0f;
        m_gainSmooth = 1.0f;
        m_lastGrDb   = 0.0f;
        m_r37X1      = 0.0f;
        m_r37Y1      = 0.0f;
        m_grGray.Reset();
        m_grGrayHalf.Reset();
        m_audioStage[0].Reset();
        m_audioStage[1].Reset();
        m_triode[0].Reset();
        m_triode[1].Reset();
    }

    // Processes mono buffer in-place when amount > 0.01f; returns minimum gain reduction (<= 0 dB).
    float ProcessBlock(float* buf, uint32_t numFrames, float amount,
                       float* outGainTrace = nullptr, float bPlus = 1.0f) noexcept {
        return ProcessStereoBlock(buf, nullptr, numFrames, amount, outGainTrace, bPlus);
    }

    // Processes mono or stereo-linked buffers in-place; returns minimum gain reduction (<= 0 dB).
    float ProcessStereoBlock(float* left, float* right, uint32_t numFrames, float amount,
                             float* outGainTrace = nullptr, float bPlus = 1.0f) noexcept {
        if (!left || numFrames == 0 || amount <= 0.01f) {
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
        const float r37Blend = std::clamp(kLa2aR37Constants.sidechainBlend, 0.0f, 0.5f);

        constexpr float kDbPerLog2 = 6.02059991f;
        constexpr float kLog2Per20 = 0.166096405f;

        const bool decimate2x = (m_fs >= 170000.0f) && (outGainTrace == nullptr);
        const float effDtMs = decimate2x ? (2.0f * m_dtMs) : m_dtMs;
        const float effAtkCoeff = decimate2x ? std::min(1.0f, 2.0f * m_atkCoeff) : m_atkCoeff;
        const float effChargeCoeff = decimate2x ? std::min(1.0f, 2.0f * m_chargeCoeff) : m_chargeCoeff;
        const float effSlowRel = effDtMs / (m_const.tauRelSlowMs + effDtMs);
        const float r37B0 = decimate2x ? m_r37HalfB0 : m_r37B0;
        const float r37B1 = decimate2x ? m_r37HalfB1 : m_r37B1;
        const float r37A1 = decimate2x ? m_r37HalfA1 : m_r37A1;

        float minGain = 1.0f;
        for (uint32_t i = 0; i < numFrames; ++i) {
            const float xL = left[i];
            const float xR = right ? right[i] : xL;

            if (!decimate2x || ((i & 1u) == 0u)) {
                const float scAudio = right ? ((std::fabs(xL) >= std::fabs(xR)) ? xL : xR) : xL;

                // Signed R37 HF pre-emphasis filter in the sidechain path before EL full-wave rectifier
                const float r37Y = r37B0 * scAudio + r37B1 * m_r37X1 - r37A1 * m_r37Y1;
                m_r37X1 = scAudio;
                m_r37Y1 = r37Y;
                const float detEff = std::fabs(scAudio + r37Blend * (r37Y - scAudio));

                const float inDb = kDbPerLog2 * FastLog2(std::max(detEff, 1.0e-5f));
                const float over = inDb - threshDb;

                float targetCellDrive = 0.0f;
                if (over > 0.5f * kneeDb) {
                    targetCellDrive = over * slope;
                } else if (over > -0.5f * kneeDb) {
                    const float t = over + 0.5f * kneeDb;
                    targetCellDrive = slope * (t * t) / (2.0f * kneeDb);
                }

                const float slowCoeff = (targetCellDrive > m_gSlow) ? effChargeCoeff : effSlowRel;
                m_gSlow += (targetCellDrive - m_gSlow) * slowCoeff;

                const float memFrac = m_gSlow / (4.0f + m_gSlow);
                const float tauEffMs = m_const.tauRelFastMs + (m_const.tauRelSlowMs - m_const.tauRelFastMs) * memFrac;
                const float fastRelCoeff = effDtMs / (tauEffMs + effDtMs);
                const float fastCoeff = (targetCellDrive > m_gFast) ? effAtkCoeff : fastRelCoeff;
                m_gFast += (targetCellDrive - m_gFast) * fastCoeff;

                // Continuous-time 8-state GR residual SSM adjusts optical cell attenuation cleanly
                const float residualDb = decimate2x ? m_grGrayHalf.Process(m_gFast) : m_grGray.Process(m_gFast);
                const float corrGrDb = std::max(0.0f, m_gFast + residualDb);
                m_gainSmooth = FastExp2(-corrGrDb * kLog2Per20);
            }

            const float gainLin = m_gainSmooth;
            // Attenuator output -> HA-100X/HS-56 audio-domain CT-SSM + coupled 12AX7A B+ rail polish
            const float gainedL = xL * gainLin;
            const float xfrmL = m_audioStage[0].Process(gainedL);
            const float tubeL = m_triode[0].Process(xfrmL, bPlus, 0.70f);
            left[i] = xfrmL + (tubeL - xfrmL) * (0.20f * amt);

            if (right) {
                const float gainedR = xR * gainLin;
                const float xfrmR = m_audioStage[1].Process(gainedR);
                const float tubeR = m_triode[1].Process(xfrmR, bPlus, 0.70f);
                right[i] = xfrmR + (tubeR - xfrmR) * (0.20f * amt);
            }

            if (outGainTrace) outGainTrace[i] = gainLin;
            minGain = std::min(minGain, gainLin);
        }

        m_lastGrDb = kDbPerLog2 * FastLog2(std::max(minGain, 1.0e-5f));
        return m_lastGrDb;
    }

    float GetLastGrDb() const noexcept { return m_lastGrDb; }

private:
    La2aT4bConstants m_const{};
    float m_fs          = 48000.0f;
    float m_dtMs        = 1.0f / 48.0f;
    float m_atkCoeff    = 0.002f;
    float m_chargeCoeff = 0.0001f;
    float m_r37B0 = 1.0f, m_r37B1 = 0.0f, m_r37A1 = 0.0f;
    float m_r37HalfW0 = 0.1f, m_r37HalfB0 = 1.0f, m_r37HalfB1 = 0.0f, m_r37HalfA1 = 0.0f;
    float m_r37X1 = 0.0f, m_r37Y1 = 0.0f;
    float m_gFast       = 0.0f;
    float m_gSlow       = 0.0f;
    float m_gainSmooth  = 1.0f;
    float m_lastGrDb    = 0.0f;
    La2aGrayBoxResidual m_grGray;
    La2aGrayBoxResidual m_grGrayHalf;
    La2aAudioStageModel m_audioStage[2];
    TriodeGridBiasStage m_triode[2];
};

} // namespace stoneridge::vocal
