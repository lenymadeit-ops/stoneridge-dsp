#pragma once

// Unified rate-adaptive analog oversampling domain (~192 kHz target):
//   4x @ 44.1 / 48 kHz, 2x @ 88.2 / 96 kHz, 1x @ 176.4 / 192 kHz (capped at 1x >=88.2k in ECO mode).
// Linear MicLocker curves/headbasket and ConsoleEq run at base rate; nonlinear MicLocker
// head-amp/BAOAB, Preamp, 1176 JFET, and LA-2A T4B run inside the ~192 kHz oversampling domain.
// Zero heap allocations, zero locks.

#include "vocal/Adaa2MolecularShaper.h"
#include "vocal/MicLocker.h"
#include "vocal/Oversampler.h"
#include "vocal/PreampAndConsole.h"
#include "vocal/SimdKernels.h"
#include "vocal/TubeAndRailPhysics.h"
#include "vocal/VocalDynamics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace stoneridge::vocal {

struct AnalogRackParams {
    uint32_t micModel              = 0u;
    float    virtualDistanceCm     = 15.0f;
    uint32_t preampModel           = 0u;
    float    preampDriveDb         = 0.0f;
    float    transformerIronAmount = 0.50f;
    float    fetPeakCatchAmount    = 0.0f;
    uint32_t fetRatioMode          = 0u;
    float    vocalLevelerAmount    = 0.0f;
    uint32_t consoleEqModel        = 0u;
    float    consoleLowDb          = 0.0f;
    float    consoleMidDb          = 0.0f;
    float    consoleAirDb          = 0.0f;
    bool     wdfEqEnabled          = false;
    bool     ecoMode               = false;
};

struct AnalogRackTelemetry {
    float fetGrDb  = 0.0f;
    float optoGrDb = 0.0f;
};

class AnalogRack {
public:
    static constexpr uint32_t kMaxBlock = Oversampler::kMaxBlock;

    void Prepare(double sampleRate, bool ecoMode = false) noexcept {
        m_baseSampleRate = (sampleRate > 1000.0) ? sampleRate : 48000.0;
        m_cachedEco = ecoMode;
        const size_t maxOs = static_cast<size_t>(kMaxBlock) * Oversampler::kMaxFactor;
        m_bPlusBuf.assign(maxOs, 1.0f);
        m_dryL.assign(maxOs, 0.0f);
        m_dryR.assign(maxOs, 0.0f);
        m_baseDryL.assign(kMaxBlock, 0.0f);
        m_baseDryR.assign(kMaxBlock, 0.0f);
        ConfigureForRate(AnalogOversampleFactor(m_baseSampleRate, ecoMode));
        m_stagesInitialized = false;
        Reset();
    }

    void Reset() noexcept {
        m_monoOs.Reset();
        m_dualOs.Reset();
        m_preDynOs.Reset();
        m_rail.Reset();
        for (int ch = 0; ch < 2; ++ch) {
            m_mic[ch].Reset();
            m_preamp[ch].Reset();
            m_eq[ch].Reset();
        }
        m_fet.Reset();
        m_opto.Reset();
        m_micSleeper.Reset();
        m_preampSleeper.Reset();
        m_fetSleeper.Reset();
        m_optoSleeper.Reset();
        m_eqSleeper.Reset();
        m_stagesInitialized = false;
    }

    // Processes <= kMaxBlock frames per call (caller chunks >512 frames).
    void ProcessStereoBlock(float* left, float* right, uint32_t numFrames,
                            const AnalogRackParams& p, AnalogRackTelemetry& tel,
                            float* preDynStemOut = nullptr) noexcept {
        if (!left || numFrames == 0) return;

        // Reconfigure unified oversampling domain cleanly on live ECO toggle (zero heap allocs).
        if (p.ecoMode != m_cachedEco) {
            m_cachedEco = p.ecoMode;
            const uint32_t newFactor = AnalogOversampleFactor(m_baseSampleRate, p.ecoMode);
            if (newFactor != m_osFactor) {
                ConfigureForRate(newFactor);
            }
        }

        const bool micWanted = (p.micModel >= 1u && p.micModel <= kMicModelCount)
                            || (std::fabs(std::clamp(p.virtualDistanceCm, 5.0f, 35.0f) - MicLockerStage::kNominalDistanceCm) >= 0.25f);
        const bool preampWanted = (p.preampModel != 0u);
        const bool fetWanted = (p.fetPeakCatchAmount > 0.01f);
        const bool optoWanted = (p.vocalLevelerAmount > 0.01f);
        const bool eqWanted = (p.consoleEqModel != 0u)
                           && (std::fabs(p.consoleLowDb) >= 0.02f || std::fabs(p.consoleMidDb) >= 0.02f || std::fabs(p.consoleAirDb) >= 0.02f);

        if (!m_stagesInitialized) {
            m_stagesInitialized = true;
            m_micSleeper.ForceEnabled(micWanted);
            m_preampSleeper.ForceEnabled(preampWanted);
            m_fetSleeper.ForceEnabled(fetWanted);
            m_optoSleeper.ForceEnabled(optoWanted);
            m_eqSleeper.ForceEnabled(eqWanted);
        } else {
            m_micSleeper.SetEnabled(micWanted);
            m_preampSleeper.SetEnabled(preampWanted);
            m_fetSleeper.SetEnabled(fetWanted);
            m_optoSleeper.SetEnabled(optoWanted);
            m_eqSleeper.SetEnabled(eqWanted);
        }

        const bool micRun    = !m_micSleeper.IsAsleep();
        const bool preampRun = !m_preampSleeper.IsAsleep();
        const bool fetRun    = !m_fetSleeper.IsAsleep();
        const bool optoRun   = !m_optoSleeper.IsAsleep();
        const bool eqRun     = !m_eqSleeper.IsAsleep();

        // Pure 0% CPU fast bypass when all 5 analog stages are settled asleep
        if (!micRun && !preampRun && !fetRun && !optoRun && !eqRun) {
            if (preDynStemOut) {
                for (uint32_t i = 0; i < numFrames; ++i) preDynStemOut[i] = left[i];
            }
            return;
        }

        const bool needOsDomain = micRun || preampRun || fetRun || optoRun;

        uint32_t offset = 0;
        while (offset < numFrames) {
            const uint32_t chunk = std::min<uint32_t>(numFrames - offset, kMaxBlock);
            const uint32_t osN = chunk * m_osFactor;
            float* cL = left + offset;
            float* cR = right ? (right + offset) : nullptr;

            auto saveBaseDry = [&]() noexcept {
                for (uint32_t i = 0; i < chunk; ++i) {
                    m_baseDryL[i] = cL[i];
                    if (cR) m_baseDryR[i] = cR[i];
                }
            };
            auto applyBaseSleeper = [&](StageCrossfadeSleeper& sl) noexcept {
                for (uint32_t i = 0; i < chunk; ++i) {
                    const float g = sl.Next();
                    cL[i] = m_baseDryL[i] + (cL[i] - m_baseDryL[i]) * g;
                    if (cR) cR[i] = m_baseDryR[i] + (cR[i] - m_baseDryR[i]) * g;
                }
            };

            // 1A. MicLocker Linear EQ + Proximity + 4-Tap Headbasket at Base Rate (fs)
            if (micRun) {
                const bool fading = (m_micSleeper.CurrentGain() < 0.9999f) || !micWanted;
                if (fading) saveBaseDry();
                m_mic[0].ProcessLinearBlock(cL, chunk, p.micModel, p.virtualDistanceCm);
                if (cR) m_mic[1].ProcessLinearBlock(cR, chunk, p.micModel, p.virtualDistanceCm);
                if (fading) applyBaseSleeper(m_micSleeper);
            }

            if (needOsDomain) {
                float* hiL = nullptr;
                float* hiR = nullptr;

                if (cR) {
                    m_dualOs.UpsampleStereo(cL, cR, chunk, hiL, hiR);
                } else {
                    hiL = m_monoOs.Upsample(cL, chunk);
                }

                // Compute shared B+ reservoir curve once across chunk from signal demand
                const float driveScale = 1.0f + (preampRun ? (p.preampDriveDb / 18.0f) : 0.0f) + (optoRun ? p.vocalLevelerAmount * 0.4f : 0.0f);
                for (uint32_t i = 0; i < osN; ++i) {
                    const float pk = cR ? std::max(std::fabs(hiL[i]), std::fabs(hiR[i])) : std::fabs(hiL[i]);
                    m_bPlusBuf[i] = m_rail.Process(std::min(1.0f, pk * driveScale));
                }
                const float chunkBPlus = m_bPlusBuf[osN - 1u];

                auto saveDry = [&]() noexcept {
                    for (uint32_t i = 0; i < osN; ++i) {
                        m_dryL[i] = hiL[i];
                        if (cR) m_dryR[i] = hiR[i];
                    }
                };
                auto applySleeper = [&](StageCrossfadeSleeper& sl) noexcept {
                    for (uint32_t i = 0; i < osN; ++i) {
                        const float g = sl.Next();
                        hiL[i] = m_dryL[i] + (hiL[i] - m_dryL[i]) * g;
                        if (cR) hiR[i] = m_dryR[i] + (hiR[i] - m_dryR[i]) * g;
                    }
                };

                // 1B. MicLocker Nonlinear HeadAmp + BAOAB Membrane inside ~192 kHz domain
                if (micRun) {
                    m_mic[0].ProcessNonlinearBlock(hiL, osN, p.micModel, chunkBPlus);
                    if (cR) m_mic[1].ProcessNonlinearBlock(hiR, osN, p.micModel, chunkBPlus);
                }

                // 2. Ferromagnetic Preamp Stage (JA series transformer + Koren V76 triode)
                if (preampRun) {
                    const bool fading = (m_preampSleeper.CurrentGain() < 0.9999f) || !preampWanted;
                    if (fading) saveDry();
                    m_preamp[0].ProcessBlock(hiL, osN, p.preampModel, p.preampDriveDb, p.transformerIronAmount, chunkBPlus);
                    if (cR) m_preamp[1].ProcessBlock(hiR, osN, p.preampModel, p.preampDriveDb, p.transformerIronAmount, chunkBPlus);
                    if (fading) applySleeper(m_preampSleeper);
                }

                // Optional AsioPreDynamicsStem tap before dynamics/EQ stages
                if (preDynStemOut) {
                    if (m_osFactor == 1u) {
                        for (uint32_t i = 0; i < chunk; ++i) preDynStemOut[offset + i] = hiL[i];
                    } else {
                        float* stemHi = m_preDynOs.HighBuffer();
                        for (uint32_t i = 0; i < osN; ++i) stemHi[i] = hiL[i];
                        m_preDynOs.Downsample(preDynStemOut + offset, chunk);
                    }
                }

                // 3. Serial 1176 Feedback FET (Shichman-Hodges + British mode, stereo-linked)
                if (fetRun) {
                    const bool fading = (m_fetSleeper.CurrentGain() < 0.9999f) || !fetWanted;
                    if (fading) saveDry();
                    const float gr = m_fet.ProcessStereoBlock(hiL, cR ? hiR : nullptr, osN, p.fetPeakCatchAmount, p.fetRatioMode);
                    tel.fetGrDb = std::min(tel.fetGrDb, gr);
                    if (fading) applySleeper(m_fetSleeper);
                }

                // 4. Serial LA-2A T4B Opto (R37 + CdS + GR CT-SSM + HA-100X/12AX7A, stereo-linked)
                if (optoRun) {
                    const bool fading = (m_optoSleeper.CurrentGain() < 0.9999f) || !optoWanted;
                    if (fading) saveDry();
                    const float gr = m_opto.ProcessStereoBlock(hiL, cR ? hiR : nullptr, osN, p.vocalLevelerAmount, nullptr, chunkBPlus);
                    tel.optoGrDb = std::min(tel.optoGrDb, gr);
                    if (fading) applySleeper(m_optoSleeper);
                }

                // Apply B+ dynamic sag cleanly onto output inside unified domain when preamp/tube active
                if (preampRun || optoRun) {
                    for (uint32_t i = 0; i < osN; ++i) {
                        hiL[i] *= m_bPlusBuf[i];
                        if (cR) hiR[i] *= m_bPlusBuf[i];
                    }
                }

                if (cR) {
                    m_dualOs.DownsampleStereo(cL, cR, chunk);
                } else {
                    m_monoOs.Downsample(cL, chunk);
                }
            } else if (preDynStemOut) {
                for (uint32_t i = 0; i < chunk; ++i) preDynStemOut[offset + i] = cL[i];
            }

            // 5. Console EQ Stage at Base Rate (bilinearly warped WDF Passive EQP-1A / ZdfTptFilter)
            if (eqRun) {
                const bool fading = (m_eqSleeper.CurrentGain() < 0.9999f) || !eqWanted;
                if (fading) saveBaseDry();
                m_eq[0].ProcessBlock(cL, chunk, p.consoleEqModel, p.consoleLowDb, p.consoleMidDb, p.consoleAirDb, p.wdfEqEnabled);
                if (cR) {
                    m_eq[1].ProcessBlock(cR, chunk, p.consoleEqModel, p.consoleLowDb, p.consoleMidDb, p.consoleAirDb, p.wdfEqEnabled);
                }
                if (fading) applyBaseSleeper(m_eqSleeper);
            }

            offset += chunk;
        }
    }

    uint32_t CurrentOversampleFactor() const noexcept { return m_osFactor; }

private:
    void ConfigureForRate(uint32_t factor) noexcept {
        m_osFactor = factor;
        m_osRate = m_baseSampleRate * static_cast<double>(factor);
        m_monoOs.Prepare(factor);
        m_dualOs.Prepare(factor);
        m_preDynOs.Prepare(factor);
        m_rail.Prepare(m_osRate);
        for (int ch = 0; ch < 2; ++ch) {
            m_mic[ch].PrepareSplit(m_baseSampleRate, m_osRate);
            m_preamp[ch].Prepare(m_osRate, false);
            m_eq[ch].Prepare(m_baseSampleRate);
        }
        m_fet.Prepare(m_osRate, false);
        m_opto.Prepare(m_osRate);
        m_micSleeper.Prepare(m_baseSampleRate);
        m_preampSleeper.Prepare(m_osRate);
        m_fetSleeper.Prepare(m_osRate);
        m_optoSleeper.Prepare(m_osRate);
        m_eqSleeper.Prepare(m_baseSampleRate);
    }

    double m_baseSampleRate = 48000.0;
    double m_osRate = 192000.0;
    uint32_t m_osFactor = 4u;
    bool m_cachedEco = false;
    bool m_stagesInitialized = false;

    Oversampler m_monoOs;
    DualOversampler m_dualOs;
    Oversampler m_preDynOs;
    PowerSupplyRail m_rail;

    std::array<MicLockerStage, 2> m_mic;
    std::array<PreampStage, 2>    m_preamp;
    Fet1176Stage                  m_fet;
    OptoLa2aT4bStage              m_opto;
    std::array<ConsoleEqStage, 2> m_eq;

    StageCrossfadeSleeper m_micSleeper;
    StageCrossfadeSleeper m_preampSleeper;
    StageCrossfadeSleeper m_fetSleeper;
    StageCrossfadeSleeper m_optoSleeper;
    StageCrossfadeSleeper m_eqSleeper;

    std::vector<float> m_bPlusBuf;
    std::vector<float> m_dryL;
    std::vector<float> m_dryR;
    std::vector<float> m_baseDryL;
    std::vector<float> m_baseDryR;
};

} // namespace stoneridge::vocal
