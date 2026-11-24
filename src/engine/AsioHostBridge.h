#pragma once

#include "asio/asio_abi.h"
#include "dsp/ControlRoomMonitor.h"
#include "dsp/InputProcessor.h"
#include "engine/AudioFormatConvert.h"
#include "ipc/SharedState.h"
#include "vocal/VoiceLoopBuffer.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace stoneridge {

struct AsioDispatchResult {
    const float* outL = nullptr;
    const float* outR = nullptr;
};

struct MonitorAuditionContext {
    vocal::VoiceLoopBuffer voiceLoop;
    InputProcessor         auditionDsp;
    std::array<float, 2048> phraseBuf{};
    std::array<float, 2048> monRawL{};
    std::array<float, 2048> monRawR{};
    uint32_t adcClipCleanBlocks = 0;
    uint32_t prevLoopCmd        = 0;
    double   preparedRate       = 0.0;

    void Prepare(double sampleRate) noexcept {
        const double rate = (sampleRate > 1000.0) ? sampleRate : 48000.0;
        if (preparedRate != rate) {
            voiceLoop.Prepare(rate);
            auditionDsp.Prepare(rate, 2);
            preparedRate = rate;
        }
        adcClipCleanBlocks = 0;
        prevLoopCmd = 0;
    }

    void Reset() noexcept {
        voiceLoop.Reset();
        auditionDsp.Reset();
        phraseBuf.fill(0.0f);
        monRawL.fill(0.0f);
        monRawR.fill(0.0f);
        adcClipCleanBlocks = 0;
        prevLoopCmd = 0;
    }
};

// Shared zero-allocation host bridge between audio backends (WdmKsStream / WasapiExclusiveStream),
// InputProcessor DSP, 1-Click Room-Noise Calibrator, and FL Studio's ASIOCallbacks.
inline AsioDispatchResult ProcessAsioHostBlock(SharedState* sharedState,
                                               InputProcessor& inputDsp,
                                               float* inL,
                                               float* inR,
                                               float* asioOutL,
                                               float* asioOutR,
                                               float* monWorkL,
                                               float* monWorkR,
                                               uint32_t bufferSize,
                                               double sampleRate,
                                               uint64_t samplePos,
                                               uint64_t nowNs,
                                               uint32_t doubleBufIdx,
                                               ASIOCallbacks* callbacks,
                                               ControlRoomMonitor* ctrlRoom = nullptr,
                                               MonitorAuditionContext* auditionCtx = nullptr) noexcept {
    ControlRoomParams crParams{};
    InputDspParams dspParams{};

    if (sharedState) {
        uint32_t expectedLearn = static_cast<uint32_t>(LearnState::StartRoom);
        if (sharedState->learnState.compare_exchange_strong(
                expectedLearn, static_cast<uint32_t>(LearnState::Sampling), std::memory_order_acq_rel)) {
            sharedState->preampCalibActive.store(false, std::memory_order_relaxed);
            inputDsp.StartLearnRoomNoise();
        }
        uint32_t expectedPreamp = static_cast<uint32_t>(LearnState::StartPreampLevel);
        if (sharedState->learnState.compare_exchange_strong(
                expectedPreamp, static_cast<uint32_t>(LearnState::Sampling), std::memory_order_acq_rel)) {
            sharedState->preampCalibActive.store(true, std::memory_order_relaxed);
            inputDsp.StartPreampAutoLevel(
                sharedState->hardwarePgaGainDb.load(std::memory_order_relaxed),
                sharedState->hasHardwarePga.load(std::memory_order_relaxed));
        }

        dspParams.gainDb                    = sharedState->inputGainDb.load(std::memory_order_relaxed);
        dspParams.mute                      = sharedState->inputMute.load(std::memory_order_relaxed) ||
                                              sharedState->hardwareMuted.load(std::memory_order_relaxed);
        dspParams.softClipEnabled           = sharedState->softClipEnabled.load(std::memory_order_relaxed);
        dspParams.audioHpfEnabled           = sharedState->audioHpfEnabled.load(std::memory_order_relaxed);
        dspParams.gateEnabled               = sharedState->gateEnabled.load(std::memory_order_relaxed);
        dspParams.gateLookAheadEnabled      = sharedState->gateLookAheadEnabled.load(std::memory_order_relaxed);
        dspParams.gateThresholdDb           = sharedState->gateThresholdDb.load(std::memory_order_relaxed);
        dspParams.gateHysteresisDb          = sharedState->gateHysteresisDb.load(std::memory_order_relaxed);
        dspParams.gateAttackMs              = sharedState->gateAttackMs.load(std::memory_order_relaxed);
        dspParams.gateHoldMs                = sharedState->gateHoldMs.load(std::memory_order_relaxed);
        dspParams.gateReleaseMs             = sharedState->gateReleaseMs.load(std::memory_order_relaxed);
        dspParams.gateRangeDb               = sharedState->gateRangeDb.load(std::memory_order_relaxed);
        dspParams.sidechainHpfHz            = sharedState->gateSidechainHpfHz.load(std::memory_order_relaxed);
        dspParams.humNotchFreqHz            = sharedState->humNotchFreqHz.load(std::memory_order_relaxed);
        dspParams.capsuleEqEnabled          = sharedState->capsuleEqEnabled.load(std::memory_order_relaxed);
        dspParams.capsuleCorrectionEnabled  = sharedState->capsuleCorrectionEnabled.load(std::memory_order_relaxed);
        dspParams.capsuleCorrectionStrength = sharedState->capsuleCorrectionStrength.load(std::memory_order_relaxed);
        dspParams.deEsserAmount             = sharedState->deEsserAmount.load(std::memory_order_relaxed);
        dspParams.plosiveGuardEnabled       = sharedState->plosiveGuardEnabled.load(std::memory_order_relaxed);
        dspParams.micModel                  = sharedState->micModel.load(std::memory_order_relaxed);
        dspParams.virtualDistanceCm         = sharedState->virtualDistanceCm.load(std::memory_order_relaxed);
        dspParams.preampModel               = sharedState->preampModel.load(std::memory_order_relaxed);
        dspParams.preampDriveDb             = sharedState->preampDriveDb.load(std::memory_order_relaxed);
        dspParams.transformerIronAmount     = sharedState->transformerIronAmount.load(std::memory_order_relaxed);
        dspParams.fetPeakCatchAmount        = sharedState->fetPeakCatchAmount.load(std::memory_order_relaxed);
        dspParams.fetRatioMode              = sharedState->fetRatioMode.load(std::memory_order_relaxed);
        dspParams.vocalLevelerAmount        = sharedState->vocalLevelerAmount.load(std::memory_order_relaxed);
        dspParams.consoleEqModel            = sharedState->consoleEqModel.load(std::memory_order_relaxed);
        dspParams.consoleLowDb              = sharedState->consoleLowDb.load(std::memory_order_relaxed);
        dspParams.consoleMidDb              = sharedState->consoleMidDb.load(std::memory_order_relaxed);
        dspParams.consoleAirDb              = sharedState->consoleAirDb.load(std::memory_order_relaxed);
        dspParams.dualTrackSafetyMode       = sharedState->dualTrackSafetyMode.load(std::memory_order_relaxed);
        dspParams.mic2RoutingMode           = sharedState->mic2RoutingMode.load(std::memory_order_relaxed);
        dspParams.abLevelMatchedBypass      = sharedState->abLevelMatchedBypass.load(std::memory_order_relaxed);
        dspParams.abAuditionMonitorOnly     = sharedState->abAuditionMonitorOnly.load(std::memory_order_relaxed);
        dspParams.freezeAbxGain             = sharedState->freezeAbxGain.load(std::memory_order_relaxed);
        dspParams.molecularClip             = sharedState->molecularClip.load(std::memory_order_relaxed);
        dspParams.wdfEqEnabled              = sharedState->wdfEqEnabled.load(std::memory_order_relaxed);
        dspParams.beautyAmount              = sharedState->beautyAmount.load(std::memory_order_relaxed);
        dspParams.contourAmt                = sharedState->contourAmt.load(std::memory_order_relaxed);
        dspParams.harmonicSculptAmt         = sharedState->harmonicSculptAmt.load(std::memory_order_relaxed);
        dspParams.deHarshAmt                = sharedState->deHarshAmt.load(std::memory_order_relaxed);
        dspParams.parallelGlueAmt           = sharedState->parallelGlueAmt.load(std::memory_order_relaxed);
        dspParams.ultrasonicLpfEnabled      = sharedState->ultrasonicLpfEnabled.load(std::memory_order_relaxed);
        dspParams.ecoMode                   = sharedState->ecoMode.load(std::memory_order_relaxed);

        crParams.directMonitorMix    = std::clamp(sharedState->directMonitorMix.load(std::memory_order_relaxed), 0.0f, 1.0f);
        crParams.monitorReverbAmount = std::clamp(sharedState->monitorReverbAmount.load(std::memory_order_relaxed), 0.0f, 1.0f);
        crParams.crossfeedEnabled    = sharedState->crossfeedEnabled.load(std::memory_order_relaxed);
        crParams.cueCarveEnabled     = sharedState->cueCarveEnabled.load(std::memory_order_relaxed);
        crParams.backingHeadroomDb   = (crParams.directMonitorMix > 0.05f)
            ? sharedState->backingHeadroomDb.load(std::memory_order_relaxed) : 0.0f;

        InputDspTelemetry telemetry{};
        inputDsp.ProcessStereo(inL, inR, bufferSize, dspParams, telemetry);

        if (sharedState->learnState.load(std::memory_order_relaxed) == static_cast<uint32_t>(LearnState::Sampling)) {
            if (sharedState->preampCalibActive.load(std::memory_order_relaxed)) {
                const auto pr = inputDsp.GetPreampAutoResult();
                sharedState->learnProgress.store(pr.progress, std::memory_order_relaxed);
                if (pr.completed) {
                    sharedState->recommendedPgaDb.store(pr.recommendedPgaDb, std::memory_order_relaxed);
                    sharedState->recommendedTrimDb.store(pr.recommendedTrimDb, std::memory_order_relaxed);
                    sharedState->hardwarePgaGainDb.store(pr.recommendedPgaDb, std::memory_order_relaxed);
                    sharedState->inputGainDb.store(pr.recommendedTrimDb, std::memory_order_relaxed);
                    sharedState->hardwareCtrlCounter.fetch_add(1, std::memory_order_release);
                    sharedState->preampCalibActive.store(false, std::memory_order_relaxed);
                    sharedState->learnState.store(static_cast<uint32_t>(LearnState::Completed), std::memory_order_release);
                }
            } else {
                const auto lr = inputDsp.GetLearnResult();
                sharedState->learnProgress.store(lr.progress, std::memory_order_relaxed);
                if (lr.completed) {
                    sharedState->measuredNoiseFloorDb.store(lr.noiseFloorDb, std::memory_order_relaxed);
                    sharedState->gateThresholdDb.store(lr.recommendedThreshDb, std::memory_order_relaxed);
                    sharedState->gateEnabled.store(true, std::memory_order_relaxed);
                    if (lr.recommendedHumHz > 1.0f) {
                        sharedState->humNotchFreqHz.store(lr.recommendedHumHz, std::memory_order_relaxed);
                    }
                    sharedState->learnState.store(static_cast<uint32_t>(LearnState::Completed), std::memory_order_release);
                }
            }
        }

        sharedState->inputPeakDb.store(telemetry.peakDb, std::memory_order_relaxed);
        sharedState->inputRmsDb.store(telemetry.rmsDb, std::memory_order_relaxed);
        sharedState->gateGainReductionDb.store(telemetry.gateReductionDb, std::memory_order_relaxed);
        sharedState->gateIsOpen.store(telemetry.gateIsOpen, std::memory_order_relaxed);
        sharedState->deEsserReductionDb.store(telemetry.deEsserReductionDb, std::memory_order_relaxed);
        sharedState->plosiveReductionDb.store(telemetry.plosiveReductionDb, std::memory_order_relaxed);
        sharedState->fetReductionDb.store(telemetry.fetReductionDb, std::memory_order_relaxed);
        sharedState->levelerReductionDb.store(telemetry.levelerReductionDb, std::memory_order_relaxed);
        sharedState->abMatchGainDb.store(telemetry.abMatchGainDb, std::memory_order_relaxed);
        sharedState->capsuleCalibrated.store(kEm91cuCalibrationMeasured, std::memory_order_relaxed);

        uint32_t dummyClean = 0;
        uint32_t& cleanBlocks = auditionCtx ? auditionCtx->adcClipCleanBlocks : dummyClean;
        if (telemetry.adcClipDetected) {
            sharedState->adcClipDetected.store(true, std::memory_order_relaxed);
            cleanBlocks = 0;
        } else if (sharedState->adcClipDetected.load(std::memory_order_relaxed)) {
            if (++cleanBlocks > 48u) {
                sharedState->adcClipDetected.store(false, std::memory_order_relaxed);
                cleanBlocks = 0;
            }
        }
    }

    if (callbacks) {
        if (callbacks->bufferSwitchTimeInfo) {
            ASIOTime timeInfo{};
            timeInfo.timeInfo.flags             = kSystemTimeValid | kSamplePositionValid | kSampleRateValid | kSpeedValid;
            timeInfo.timeInfo.speed             = 1.0;
            timeInfo.timeInfo.sampleRate        = sampleRate;
            timeInfo.timeInfo.samplePosition.hi = static_cast<uint32_t>(samplePos >> 32);
            timeInfo.timeInfo.samplePosition.lo = static_cast<uint32_t>(samplePos & 0xFFFFFFFF);
            timeInfo.timeInfo.systemTime.hi     = static_cast<uint32_t>(nowNs >> 32);
            timeInfo.timeInfo.systemTime.lo     = static_cast<uint32_t>(nowNs & 0xFFFFFFFF);
            callbacks->bufferSwitchTimeInfo(&timeInfo, static_cast<long>(doubleBufIdx), ASIOTrue);
        } else if (callbacks->bufferSwitch) {
            callbacks->bufferSwitch(static_cast<long>(doubleBufIdx), ASIOTrue);
        }
    }

    AsioDispatchResult res{ asioOutL, asioOutR };

    bool looping = false;
    bool recording = false;
    if (auditionCtx) {
        if (auditionCtx->preparedRate != sampleRate) {
            auditionCtx->Prepare(sampleRate);
        }
        const uint32_t loopCmd = sharedState ? sharedState->voiceLoopState.load(std::memory_order_relaxed) : 0u;
        if (loopCmd != auditionCtx->prevLoopCmd) {
            if (loopCmd == 1u) auditionCtx->voiceLoop.Record();
            else if (loopCmd == 2u) auditionCtx->voiceLoop.Play();
            else auditionCtx->voiceLoop.Stop();
            auditionCtx->prevLoopCmd = loopCmd;
        }
        recording = (auditionCtx->voiceLoop.State() == vocal::VoiceLoopState::Recording);
        looping   = (auditionCtx->voiceLoop.State() == vocal::VoiceLoopState::Looping);
    }

    if (looping && crParams.directMonitorMix < 0.2f) {
        crParams.directMonitorMix = 0.85f;
    }

    const bool needCtrlRoom = (crParams.directMonitorMix > 0.001f)
                           || crParams.crossfeedEnabled
                           || looping
                           || recording;

    if (needCtrlRoom && inL && monWorkL && monWorkR) {
        const bool splitSafety = (dspParams.mic2RoutingMode != 0u) || dspParams.dualTrackSafetyMode;
        const float* micL = inL;
        const float* micR = (inR && !splitSafety) ? inR : inL;

        if (auditionCtx) {
            const uint32_t nCopy = std::min(bufferSize, static_cast<uint32_t>(auditionCtx->phraseBuf.size()));
            if (recording) {
                inputDsp.CopyDryTap(auditionCtx->phraseBuf.data(), nCopy);
                for (uint32_t i = 0; i < nCopy; ++i) {
                    auditionCtx->voiceLoop.Process(auditionCtx->phraseBuf[i]);
                }
                if (auditionCtx->voiceLoop.State() == vocal::VoiceLoopState::Looping) {
                    looping = true;
                    recording = false;
                    auditionCtx->prevLoopCmd = 2u;
                    if (sharedState) {
                        sharedState->voiceLoopState.store(2u, std::memory_order_relaxed);
                    }
                }
            } else if (looping) {
                for (uint32_t i = 0; i < nCopy; ++i) {
                    float s = 0.0f;
                    auditionCtx->voiceLoop.Process(s);
                    auditionCtx->phraseBuf[i] = s;
                }
                InputDspParams loopParams = dspParams;
                // Audition DSP drives headphones only; never overwrite FL Studio buffers, so
                // setting abAuditionMonitorOnly=false switches cleanly between A (Chain) and B (Raw).
                loopParams.abAuditionMonitorOnly = false;
                InputDspTelemetry auditionTel{};
                auditionCtx->auditionDsp.ProcessMono(auditionCtx->phraseBuf.data(), nCopy, loopParams, auditionTel);
                if (sharedState) {
                    sharedState->abMatchGainDb.store(auditionTel.abMatchGainDb, std::memory_order_relaxed);
                }
                micL = auditionCtx->phraseBuf.data();
                micR = auditionCtx->phraseBuf.data();
            } else if (dspParams.abLevelMatchedBypass && dspParams.abAuditionMonitorOnly) {
                inputDsp.CopyMonitorAuditionTap(auditionCtx->monRawL.data(), auditionCtx->monRawR.data(), nCopy);
                micL = auditionCtx->monRawL.data();
                micR = (inR && !splitSafety) ? auditionCtx->monRawR.data() : auditionCtx->monRawL.data();
            }
        }

        if (ctrlRoom) {
            ctrlRoom->ProcessBlock(asioOutL, asioOutR, micL, micR, monWorkL, monWorkR, bufferSize, crParams);
        } else {
            BlendDirectMonitorAvx2(asioOutL, micL, monWorkL, bufferSize, crParams.directMonitorMix);
            BlendDirectMonitorAvx2(asioOutR, micR, monWorkR, bufferSize, crParams.directMonitorMix);
        }
        res.outL = monWorkL;
        res.outR = monWorkR;
    }
    return res;
}

} // namespace stoneridge
