#pragma once

#include "ipc/SharedState.h"

#include <windows.h>
#include <shlobj.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <string>

namespace stoneridge {

inline std::wstring GetConfigFilePath() {
    wchar_t* appData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        dir = std::wstring(appData) + L"\\StoneridgeDSP";
        CoTaskMemFree(appData);
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\config.json";
    }
    return L"config.json";
}

inline void LoadConfigToSharedState(SharedState* state) {
    if (!state) return;
    std::ifstream ifs(GetConfigFilePath());
    if (!ifs.is_open()) return;

    try {
        nlohmann::json j;
        ifs >> j;
        if (j.contains("inputGainDb"))          state->inputGainDb.store(j["inputGainDb"].get<float>(), std::memory_order_relaxed);
        if (j.contains("inputMute"))            state->inputMute.store(j["inputMute"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("softClipEnabled"))      state->softClipEnabled.store(j["softClipEnabled"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("audioHpfEnabled"))      state->audioHpfEnabled.store(j["audioHpfEnabled"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("directMonitorMix"))     state->directMonitorMix.store(j["directMonitorMix"].get<float>(), std::memory_order_relaxed);
        if (j.contains("gateEnabled"))          state->gateEnabled.store(j["gateEnabled"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("gateLookAheadEnabled")) state->gateLookAheadEnabled.store(j["gateLookAheadEnabled"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("gateThresholdDb"))      state->gateThresholdDb.store(j["gateThresholdDb"].get<float>(), std::memory_order_relaxed);
        if (j.contains("gateHysteresisDb"))     state->gateHysteresisDb.store(j["gateHysteresisDb"].get<float>(), std::memory_order_relaxed);
        if (j.contains("gateAttackMs"))         state->gateAttackMs.store(j["gateAttackMs"].get<float>(), std::memory_order_relaxed);
        if (j.contains("gateHoldMs"))           state->gateHoldMs.store(j["gateHoldMs"].get<float>(), std::memory_order_relaxed);
        if (j.contains("gateReleaseMs"))        state->gateReleaseMs.store(j["gateReleaseMs"].get<float>(), std::memory_order_relaxed);
        if (j.contains("gateRangeDb"))          state->gateRangeDb.store(j["gateRangeDb"].get<float>(), std::memory_order_relaxed);
        if (j.contains("gateSidechainHpfHz"))   state->gateSidechainHpfHz.store(j["gateSidechainHpfHz"].get<float>(), std::memory_order_relaxed);
        if (j.contains("humNotchFreqHz"))       state->humNotchFreqHz.store(j["humNotchFreqHz"].get<float>(), std::memory_order_relaxed);
        if (j.contains("capsuleEqEnabled"))     state->capsuleEqEnabled.store(j["capsuleEqEnabled"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("capsuleCorrectionEnabled"))  state->capsuleCorrectionEnabled.store(j["capsuleCorrectionEnabled"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("capsuleCorrectionStrength")) state->capsuleCorrectionStrength.store(j["capsuleCorrectionStrength"].get<float>(), std::memory_order_relaxed);
        if (j.contains("deEsserAmount"))        state->deEsserAmount.store(j["deEsserAmount"].get<float>(), std::memory_order_relaxed);
        if (j.contains("plosiveGuardEnabled"))  state->plosiveGuardEnabled.store(j["plosiveGuardEnabled"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("micModel"))             state->micModel.store(j["micModel"].get<uint32_t>(), std::memory_order_relaxed);
        if (j.contains("virtualDistanceCm"))    state->virtualDistanceCm.store(j["virtualDistanceCm"].get<float>(), std::memory_order_relaxed);
        if (j.contains("fetPeakCatchAmount"))   state->fetPeakCatchAmount.store(j["fetPeakCatchAmount"].get<float>(), std::memory_order_relaxed);
        if (j.contains("fetRatioMode"))         state->fetRatioMode.store(j["fetRatioMode"].get<uint32_t>(), std::memory_order_relaxed);
        if (j.contains("vocalLevelerAmount"))   state->vocalLevelerAmount.store(j["vocalLevelerAmount"].get<float>(), std::memory_order_relaxed);
        if (j.contains("preampModel"))          state->preampModel.store(j["preampModel"].get<uint32_t>(), std::memory_order_relaxed);
        if (j.contains("preampDriveDb"))        state->preampDriveDb.store(j["preampDriveDb"].get<float>(), std::memory_order_relaxed);
        if (j.contains("transformerIronAmount")) state->transformerIronAmount.store(j["transformerIronAmount"].get<float>(), std::memory_order_relaxed);
        if (j.contains("consoleEqModel"))       state->consoleEqModel.store(j["consoleEqModel"].get<uint32_t>(), std::memory_order_relaxed);
        if (j.contains("consoleLowDb"))         state->consoleLowDb.store(j["consoleLowDb"].get<float>(), std::memory_order_relaxed);
        if (j.contains("consoleMidDb"))         state->consoleMidDb.store(j["consoleMidDb"].get<float>(), std::memory_order_relaxed);
        if (j.contains("consoleAirDb"))         state->consoleAirDb.store(j["consoleAirDb"].get<float>(), std::memory_order_relaxed);
        if (j.contains("dualTrackSafetyMode"))    state->dualTrackSafetyMode.store(j["dualTrackSafetyMode"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("mic2RoutingMode"))        state->mic2RoutingMode.store(j["mic2RoutingMode"].get<uint32_t>(), std::memory_order_relaxed);
        if (j.contains("monitorReverbAmount"))    state->monitorReverbAmount.store(j["monitorReverbAmount"].get<float>(), std::memory_order_relaxed);
        if (j.contains("crossfeedEnabled"))       state->crossfeedEnabled.store(j["crossfeedEnabled"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("cueCarveEnabled"))        state->cueCarveEnabled.store(j["cueCarveEnabled"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("ultrasonicLpfEnabled"))   state->ultrasonicLpfEnabled.store(j["ultrasonicLpfEnabled"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("driftAsrcEnabled"))       state->driftAsrcEnabled.store(j["driftAsrcEnabled"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("dacIspGuardEnabled"))     state->dacIspGuardEnabled.store(j["dacIspGuardEnabled"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("dacDitherShaperEnabled")) state->dacDitherShaperEnabled.store(j["dacDitherShaperEnabled"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("hardwareDacBalanceDb"))   state->hardwareDacBalanceDb.store(j["hardwareDacBalanceDb"].get<float>(), std::memory_order_relaxed);
        if (j.contains("autoPadOnClipEnabled"))   state->autoPadOnClipEnabled.store(j["autoPadOnClipEnabled"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("engineMode"))           state->engineMode.store(j["engineMode"].get<uint32_t>(), std::memory_order_relaxed);
        if (j.contains("ksIrpCount"))           state->ksIrpCount.store(j["ksIrpCount"].get<uint32_t>(), std::memory_order_relaxed);
        if (j.contains("autopilotEnabled"))     state->autopilotEnabled.store(j["autopilotEnabled"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("requestedBufferSize"))  state->requestedBufferSize.store(j["requestedBufferSize"].get<uint32_t>(), std::memory_order_relaxed);
        if (j.contains("requestedSampleRate"))  state->requestedSampleRate.store(j["requestedSampleRate"].get<double>(), std::memory_order_relaxed);
        if (j.contains("pCorePriority"))        state->pCorePriority.store(j["pCorePriority"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("adaptiveXRunGuard"))    state->adaptiveXRunGuard.store(j["adaptiveXRunGuard"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("themeMode"))            state->themeMode.store(j["themeMode"].get<uint32_t>(), std::memory_order_relaxed);
        if (j.contains("hardwarePgaGainDb"))    state->hardwarePgaGainDb.store(j["hardwarePgaGainDb"].get<float>(), std::memory_order_relaxed);
        if (j.contains("hardwareDacGainDb"))    state->hardwareDacGainDb.store(j["hardwareDacGainDb"].get<float>(), std::memory_order_relaxed);
        if (j.contains("abLevelMatchedBypass")) state->abLevelMatchedBypass.store(j["abLevelMatchedBypass"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("abAuditionMonitorOnly")) state->abAuditionMonitorOnly.store(j["abAuditionMonitorOnly"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("ecoMode"))              state->ecoMode.store(j["ecoMode"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("beautyAmount"))         state->beautyAmount.store(j["beautyAmount"].get<float>(), std::memory_order_relaxed);
        if (j.contains("wdfEqEnabled"))         state->wdfEqEnabled.store(j["wdfEqEnabled"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("molecularClip"))        state->molecularClip.store(j["molecularClip"].get<bool>(), std::memory_order_relaxed);
        if (j.contains("voiceLoopState"))       state->voiceLoopState.store(j["voiceLoopState"].get<uint32_t>(), std::memory_order_relaxed);
    } catch (...) {
    }
}

inline void SaveSharedStateToConfig(const SharedState* state) {
    if (!state) return;
    nlohmann::json j = {
        { "inputGainDb",          state->inputGainDb.load(std::memory_order_relaxed) },
        { "inputMute",            state->inputMute.load(std::memory_order_relaxed) },
        { "softClipEnabled",      state->softClipEnabled.load(std::memory_order_relaxed) },
        { "audioHpfEnabled",      state->audioHpfEnabled.load(std::memory_order_relaxed) },
        { "directMonitorMix",     state->directMonitorMix.load(std::memory_order_relaxed) },
        { "gateEnabled",          state->gateEnabled.load(std::memory_order_relaxed) },
        { "gateLookAheadEnabled", state->gateLookAheadEnabled.load(std::memory_order_relaxed) },
        { "gateThresholdDb",      state->gateThresholdDb.load(std::memory_order_relaxed) },
        { "gateHysteresisDb",     state->gateHysteresisDb.load(std::memory_order_relaxed) },
        { "gateAttackMs",         state->gateAttackMs.load(std::memory_order_relaxed) },
        { "gateHoldMs",           state->gateHoldMs.load(std::memory_order_relaxed) },
        { "gateReleaseMs",        state->gateReleaseMs.load(std::memory_order_relaxed) },
        { "gateRangeDb",          state->gateRangeDb.load(std::memory_order_relaxed) },
        { "gateSidechainHpfHz",   state->gateSidechainHpfHz.load(std::memory_order_relaxed) },
        { "humNotchFreqHz",       state->humNotchFreqHz.load(std::memory_order_relaxed) },
        { "capsuleEqEnabled",     state->capsuleEqEnabled.load(std::memory_order_relaxed) },
        { "capsuleCorrectionEnabled",  state->capsuleCorrectionEnabled.load(std::memory_order_relaxed) },
        { "capsuleCorrectionStrength", state->capsuleCorrectionStrength.load(std::memory_order_relaxed) },
        { "deEsserAmount",        state->deEsserAmount.load(std::memory_order_relaxed) },
        { "plosiveGuardEnabled",  state->plosiveGuardEnabled.load(std::memory_order_relaxed) },
        { "micModel",             state->micModel.load(std::memory_order_relaxed) },
        { "virtualDistanceCm",    state->virtualDistanceCm.load(std::memory_order_relaxed) },
        { "fetPeakCatchAmount",   state->fetPeakCatchAmount.load(std::memory_order_relaxed) },
        { "fetRatioMode",         state->fetRatioMode.load(std::memory_order_relaxed) },
        { "vocalLevelerAmount",   state->vocalLevelerAmount.load(std::memory_order_relaxed) },
        { "preampModel",          state->preampModel.load(std::memory_order_relaxed) },
        { "preampDriveDb",        state->preampDriveDb.load(std::memory_order_relaxed) },
        { "transformerIronAmount", state->transformerIronAmount.load(std::memory_order_relaxed) },
        { "consoleEqModel",       state->consoleEqModel.load(std::memory_order_relaxed) },
        { "consoleLowDb",         state->consoleLowDb.load(std::memory_order_relaxed) },
        { "consoleMidDb",         state->consoleMidDb.load(std::memory_order_relaxed) },
        { "consoleAirDb",         state->consoleAirDb.load(std::memory_order_relaxed) },
        { "dualTrackSafetyMode",    state->dualTrackSafetyMode.load(std::memory_order_relaxed) },
        { "mic2RoutingMode",        state->mic2RoutingMode.load(std::memory_order_relaxed) },
        { "monitorReverbAmount",    state->monitorReverbAmount.load(std::memory_order_relaxed) },
        { "crossfeedEnabled",       state->crossfeedEnabled.load(std::memory_order_relaxed) },
        { "cueCarveEnabled",        state->cueCarveEnabled.load(std::memory_order_relaxed) },
        { "ultrasonicLpfEnabled",   state->ultrasonicLpfEnabled.load(std::memory_order_relaxed) },
        { "driftAsrcEnabled",       state->driftAsrcEnabled.load(std::memory_order_relaxed) },
        { "dacIspGuardEnabled",     state->dacIspGuardEnabled.load(std::memory_order_relaxed) },
        { "dacDitherShaperEnabled", state->dacDitherShaperEnabled.load(std::memory_order_relaxed) },
        { "hardwareDacBalanceDb",   state->hardwareDacBalanceDb.load(std::memory_order_relaxed) },
        { "autoPadOnClipEnabled",   state->autoPadOnClipEnabled.load(std::memory_order_relaxed) },
        { "engineMode",           state->engineMode.load(std::memory_order_relaxed) },
        { "ksIrpCount",           state->ksIrpCount.load(std::memory_order_relaxed) },
        { "autopilotEnabled",     state->autopilotEnabled.load(std::memory_order_relaxed) },
        { "requestedBufferSize",  state->requestedBufferSize.load(std::memory_order_relaxed) },
        { "requestedSampleRate",  state->requestedSampleRate.load(std::memory_order_relaxed) },
        { "pCorePriority",        state->pCorePriority.load(std::memory_order_relaxed) },
        { "adaptiveXRunGuard",    state->adaptiveXRunGuard.load(std::memory_order_relaxed) },
        { "themeMode",            state->themeMode.load(std::memory_order_relaxed) },
        { "hardwarePgaGainDb",    state->hardwarePgaGainDb.load(std::memory_order_relaxed) },
        { "hardwareDacGainDb",    state->hardwareDacGainDb.load(std::memory_order_relaxed) },
        { "abLevelMatchedBypass", state->abLevelMatchedBypass.load(std::memory_order_relaxed) },
        { "abAuditionMonitorOnly", state->abAuditionMonitorOnly.load(std::memory_order_relaxed) },
        { "ecoMode",              state->ecoMode.load(std::memory_order_relaxed) },
        { "beautyAmount",         state->beautyAmount.load(std::memory_order_relaxed) },
        { "wdfEqEnabled",         state->wdfEqEnabled.load(std::memory_order_relaxed) },
        { "molecularClip",        state->molecularClip.load(std::memory_order_relaxed) },
        { "voiceLoopState",       state->voiceLoopState.load(std::memory_order_relaxed) }
    };
    std::ofstream ofs(GetConfigFilePath());
    if (ofs.is_open()) {
        ofs << j.dump(2);
    }
}

} // namespace stoneridge
