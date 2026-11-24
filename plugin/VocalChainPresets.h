#pragma once

#include "dsp/InputProcessor.h"

namespace stoneridge {

struct VocalChainPreset {
    const char* name;
    float gainDb;
    uint32_t micModel;
    uint32_t preampModel;
    float preampDriveDb;
    uint32_t consoleEqModel;
    float consoleLowDb;
    float consoleMidDb;
    float consoleAirDb;
    float fetAmount;
    uint32_t fetRatio;
    float la2a;
    float beauty;
    float deEss;
};

inline VocalChainPreset PresetByIndex(int index) noexcept {
    static const VocalChainPreset kPresets[] = {
        { "Modern Pop Gloss", 2.0f, 1u, 1u, 4.0f, 1u, 2.0f, 0.0f, 3.0f, 0.35f, 0u, 0.45f, 0.55f, 0.40f },
        { "Warm R&B Velvet", 1.5f, 3u, 3u, 5.0f, 2u, 2.5f, 1.0f, 1.0f, 0.15f, 0u, 0.60f, 0.40f, 0.25f },
        { "Punchy Rap Authority", 3.0f, 5u, 2u, 5.0f, 4u, 1.0f, 2.0f, 0.5f, 0.70f, 2u, 0.20f, 0.25f, 0.50f },
        { "Intimate Acoustic", 1.0f, 4u, 3u, 2.0f, 1u, 0.5f, 0.0f, 1.5f, 0.10f, 0u, 0.30f, 0.20f, 0.15f },
        { "Podcast Broadcast", 4.0f, 2u, 4u, 1.0f, 3u, -1.0f, 1.0f, 0.5f, 0.20f, 0u, 0.50f, 0.15f, 0.45f },
        { "Raw Reference", 0.0f, 0u, 0u, 0.0f, 0u, 0.0f, 0.0f, 0.0f, 0.0f, 0u, 0.0f, 0.0f, 0.0f },
    };
    if (index < 0) index = 0;
    if (index > 5) index = 5;
    return kPresets[index];
}

inline void ApplyVocalPreset(InputDspParams& params, const VocalChainPreset& preset) noexcept {
    params.gainDb = preset.gainDb;
    params.softClipEnabled = true;
    params.molecularClip = preset.micModel != 0u;
    params.wdfEqEnabled = preset.consoleEqModel != 0u;
    params.audioHpfEnabled = preset.micModel != 0u;
    params.micModel = preset.micModel;
    params.virtualDistanceCm = 15.0f;
    params.preampModel = preset.preampModel;
    params.preampDriveDb = preset.preampDriveDb;
    params.transformerIronAmount = 0.45f;
    params.consoleEqModel = preset.consoleEqModel;
    params.consoleLowDb = preset.consoleLowDb;
    params.consoleMidDb = preset.consoleMidDb;
    params.consoleAirDb = preset.consoleAirDb;
    params.fetPeakCatchAmount = preset.fetAmount;
    params.fetRatioMode = preset.fetRatio;
    params.vocalLevelerAmount = preset.la2a;
    params.beautyAmount = preset.beauty;
    params.deEsserAmount = preset.deEss;
    params.plosiveGuardEnabled = preset.deEss > 0.35f;
    params.gateEnabled = false;
}

} // namespace stoneridge
