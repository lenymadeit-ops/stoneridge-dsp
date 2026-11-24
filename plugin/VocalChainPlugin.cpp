// Stoneridge Vocal Chain v7.1.0 -- Shares InputProcessor & DacConditioner with Stoneridge ASIO.
#define SONORE_NUM_PARAMS 9

#include "dsp/DacConditioner.h"
#ifdef GUID_NULL
#undef GUID_NULL
#endif
#include "dsp/InputProcessor.h"
#include "plugin/VocalChainPresets.h"

#include <sonore/dsp.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {

inline float JsonExtractFloat(const std::string& json, const char* key, float fallback) {
    const std::string token = std::string("\"") + key + "\"";
    const auto pos = json.find(token);
    if (pos == std::string::npos) return fallback;
    const auto colon = json.find(':', pos + token.size());
    if (colon == std::string::npos) return fallback;
    char* endPtr = nullptr;
    const float val = std::strtof(json.c_str() + colon + 1, &endPtr);
    if (endPtr == json.c_str() + colon + 1 || !std::isfinite(val)) return fallback;
    return val;
}

inline bool JsonExtractBool(const std::string& json, const char* key, bool fallback) {
    const std::string token = std::string("\"") + key + "\"";
    const auto pos = json.find(token);
    if (pos == std::string::npos) return fallback;
    const auto colon = json.find(':', pos + token.size());
    if (colon == std::string::npos) return fallback;
    for (size_t i = colon + 1; i < json.size(); ++i) {
        const char ch = json[i];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
        if (json.compare(i, 4, "true") == 0) return true;
        if (json.compare(i, 5, "false") == 0) return false;
        break;
    }
    return fallback;
}

inline stoneridge::InputDspParams MakeDefaultLiveChain() noexcept {
    stoneridge::InputDspParams p{};
    p.softClipEnabled = true;
    p.molecularClip = true;
    p.wdfEqEnabled = true;
    return p;
}

inline bool ParseLiveTrackingChain(stoneridge::InputDspParams& outParams) {
    outParams = MakeDefaultLiveChain();
    const char* app = std::getenv("APPDATA");
    if (!app || !app[0]) return false;

    const std::string path = std::string(app) + "\\StoneridgeDSP\\live_tracking_chain.json";
    std::ifstream in(path);
    if (!in.is_open()) return false;

    std::string json;
    std::string line;
    while (std::getline(in, line)) {
        json += line;
        if (json.size() > 65536u) break;
    }
    if (json.empty()) return false;

    outParams.gainDb                    = std::clamp(JsonExtractFloat(json, "inputGainDb", 0.0f), -24.0f, 24.0f);
    outParams.softClipEnabled           = JsonExtractBool(json, "softClipEnabled", true);
    outParams.audioHpfEnabled           = JsonExtractBool(json, "audioHpfEnabled", false);
    outParams.gateEnabled               = JsonExtractBool(json, "gateEnabled", false);
    outParams.gateLookAheadEnabled      = JsonExtractBool(json, "gateLookAheadEnabled", true);
    outParams.gateThresholdDb           = std::clamp(JsonExtractFloat(json, "gateThresholdDb", -42.0f), -80.0f, 0.0f);
    outParams.gateHysteresisDb          = std::clamp(JsonExtractFloat(json, "gateHysteresisDb", 4.0f), 0.5f, 18.0f);
    outParams.gateAttackMs              = std::clamp(JsonExtractFloat(json, "gateAttackMs", 1.5f), 0.1f, 50.0f);
    outParams.gateHoldMs                = std::clamp(JsonExtractFloat(json, "gateHoldMs", 40.0f), 1.0f, 500.0f);
    outParams.gateReleaseMs             = std::clamp(JsonExtractFloat(json, "gateReleaseMs", 120.0f), 5.0f, 1000.0f);
    outParams.gateRangeDb               = std::clamp(JsonExtractFloat(json, "gateRangeDb", -60.0f), -80.0f, 0.0f);
    outParams.sidechainHpfHz            = std::clamp(JsonExtractFloat(json, "gateSidechainHpfHz", 80.0f), 20.0f, 500.0f);
    outParams.humNotchFreqHz            = std::clamp(JsonExtractFloat(json, "humNotchFreqHz", 0.0f), 0.0f, 120.0f);
    outParams.capsuleEqEnabled          = JsonExtractBool(json, "capsuleEqEnabled", false);
    outParams.capsuleCorrectionEnabled  = JsonExtractBool(json, "capsuleCorrectionEnabled", false);
    outParams.capsuleCorrectionStrength = std::clamp(JsonExtractFloat(json, "capsuleCorrectionStrength", 0.5f), 0.0f, 1.0f);
    outParams.deEsserAmount             = std::clamp(JsonExtractFloat(json, "deEsserAmount", 0.0f), 0.0f, 1.0f);
    outParams.plosiveGuardEnabled       = JsonExtractBool(json, "plosiveGuardEnabled", false);
    outParams.micModel                  = static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(JsonExtractFloat(json, "micModel", 0.0f))), 0, 8));
    outParams.virtualDistanceCm         = std::clamp(JsonExtractFloat(json, "virtualDistanceCm", 15.0f), 3.0f, 100.0f);
    outParams.fetPeakCatchAmount        = std::clamp(JsonExtractFloat(json, "fetPeakCatchAmount", 0.0f), 0.0f, 1.0f);
    outParams.fetRatioMode              = static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(JsonExtractFloat(json, "fetRatioMode", 0.0f))), 0, 2));
    outParams.vocalLevelerAmount        = std::clamp(JsonExtractFloat(json, "vocalLevelerAmount", 0.0f), 0.0f, 1.0f);
    outParams.preampModel               = static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(JsonExtractFloat(json, "preampModel", 0.0f))), 0, 8));
    outParams.preampDriveDb             = std::clamp(JsonExtractFloat(json, "preampDriveDb", 0.0f), 0.0f, 24.0f);
    outParams.transformerIronAmount     = std::clamp(JsonExtractFloat(json, "transformerIronAmount", 0.35f), 0.0f, 1.0f);
    outParams.consoleEqModel            = static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(JsonExtractFloat(json, "consoleEqModel", 0.0f))), 0, 8));
    outParams.consoleLowDb              = std::clamp(JsonExtractFloat(json, "consoleLowDb", 0.0f), -12.0f, 12.0f);
    outParams.consoleMidDb              = std::clamp(JsonExtractFloat(json, "consoleMidDb", 0.0f), -12.0f, 12.0f);
    outParams.consoleAirDb              = std::clamp(JsonExtractFloat(json, "consoleAirDb", 0.0f), -12.0f, 12.0f);
    outParams.beautyAmount              = std::clamp(JsonExtractFloat(json, "beautyAmount", 0.0f), 0.0f, 1.0f);
    outParams.contourAmt                = std::clamp(JsonExtractFloat(json, "contourAmt", 1.0f), 0.0f, 1.0f);
    outParams.harmonicSculptAmt         = std::clamp(JsonExtractFloat(json, "harmonicSculptAmt", 1.0f), 0.0f, 1.0f);
    outParams.deHarshAmt                = std::clamp(JsonExtractFloat(json, "deHarshAmt", 1.0f), 0.0f, 1.0f);
    outParams.parallelGlueAmt           = std::clamp(JsonExtractFloat(json, "parallelGlueAmt", 1.0f), 0.0f, 1.0f);
    outParams.wdfEqEnabled              = JsonExtractBool(json, "wdfEqEnabled", true);
    outParams.molecularClip             = JsonExtractBool(json, "molecularClip", true);
    outParams.ecoMode                   = JsonExtractBool(json, "ecoMode", false);
    return true;
}

} // namespace

struct SonoreDsp {
    void prepare(const sonore::ProcessSpec& spec) {
        const double nextRate = (spec.sampleRate > 1000.0) ? spec.sampleRate : 48000.0;
        if (!m_prepared || std::fabs(nextRate - sampleRate) > 1e-6) {
            sampleRate = nextRate;
            chain.Prepare(sampleRate, 2);
            safetyLimiter.Prepare(sampleRate);
            m_prepared = true;
        }
        offlineRender = spec.offline;
        // File IO happens strictly during non-RT prepare(), never inside process() or reset().
        if (!m_liveChainChecked) {
            m_hasLiveChain = ParseLiveTrackingChain(m_liveChainParams);
            m_liveChainChecked = true;
        }
        m_lastGrDb = 0.0f;
    }

    void reset() noexcept {
        chain.Reset();
        safetyLimiter.Reset();
        m_lastGrDb = 0.0f;
    }

    int latency() const noexcept {
        return 6; // DacConditioner ITU-R BS.1770-4 48-tap polyphase ISP guard delay
    }

    float gainReduction() const noexcept {
        return m_lastGrDb;
    }

    void process(sonore::AudioBlock<float>& block, const float* p) noexcept {
        const int channels = static_cast<int>(block.getNumChannels());
        const int frames   = static_cast<int>(block.getNumSamples());
        if (frames <= 0 || channels <= 0) return;

        float* left = block.getChannelPointer(0);
        if (!left) return;
        float* right = (channels > 1) ? block.getChannelPointer(1) : nullptr;

        stoneridge::InputDspParams params{};
        params.softClipEnabled = true;
        params.molecularClip   = true;
        params.wdfEqEnabled    = true;

        const float presetValue = p ? p[8] : 0.0f;
        const int presetIndex = static_cast<int>(presetValue + 0.001f);
        const bool anyKnobNonZero = p && (
            std::fabs(p[0]) > 1e-4f || std::fabs(p[1]) > 1e-4f || std::fabs(p[2]) > 1e-4f ||
            std::fabs(p[3]) > 1e-4f || std::fabs(p[4]) > 1e-4f || std::fabs(p[5]) > 1e-4f ||
            std::fabs(p[6]) > 1e-4f || std::fabs(p[7]) > 1e-4f);

        if (presetIndex >= 1 && presetIndex <= 6) {
            stoneridge::ApplyVocalPreset(params, stoneridge::PresetByIndex(presetIndex - 1));
            if (anyKnobNonZero) {
                params.preampDriveDb      = p[0] * 12.0f;
                params.gainDb             = p[1];
                params.fetPeakCatchAmount = p[2];
                params.fetRatioMode       = static_cast<uint32_t>(std::lround(std::clamp(p[3], 0.0f, 2.0f)));
                params.vocalLevelerAmount = p[4];
                params.beautyAmount       = p[5];
                params.consoleLowDb       = p[6];
                params.consoleAirDb       = p[7];
            }
        } else if (presetValue >= 0.5f && presetValue < 1.0f && m_hasLiveChain) {
            params = m_liveChainParams;
        } else {
            params.gainDb             = p ? p[1] : 0.0f;
            params.preampDriveDb      = p ? (p[0] * 12.0f) : 0.0f;
            params.preampModel        = (params.preampDriveDb > 0.05f) ? 1u : 0u;
            params.fetPeakCatchAmount = p ? p[2] : 0.0f;
            params.fetRatioMode       = p ? static_cast<uint32_t>(std::lround(std::clamp(p[3], 0.0f, 2.0f))) : 0u;
            params.vocalLevelerAmount = p ? p[4] : 0.0f;
            params.beautyAmount       = p ? p[5] : 0.0f;
            params.consoleLowDb       = p ? p[6] : 0.0f;
            params.consoleAirDb       = p ? p[7] : 0.0f;
            params.consoleEqModel     = (std::fabs(params.consoleLowDb) > 0.05f || std::fabs(params.consoleAirDb) > 0.05f) ? 1u : 0u;
        }

        if (offlineRender) {
            params.ecoMode = false;
        }

        float maxDynGrDb = 0.0f;
        int offset = 0;
        while (offset < frames) {
            const uint32_t chunk = std::min<uint32_t>(1024u, static_cast<uint32_t>(frames - offset));
            float* lPtr = left + offset;
            float* rPtr = right ? (right + offset) : nullptr;

            stoneridge::InputDspTelemetry tel{};
            if (rPtr) {
                chain.ProcessStereo(lPtr, rPtr, chunk, params, tel);
            } else {
                chain.ProcessMono(lPtr, chunk, params, tel);
            }

            stoneridge::DacTelemetry dacTel{};
            safetyLimiter.ProcessStereoFloat(lPtr, rPtr, lPtr, rPtr, chunk, true, dacTel);

            const float chunkGr = std::max({
                0.0f,
                -std::min(0.0f, tel.fetReductionDb),
                -std::min(0.0f, tel.levelerReductionDb)
            });
            maxDynGrDb = std::max(maxDynGrDb, chunkGr);
            offset += static_cast<int>(chunk);
        }
        m_lastGrDb = std::clamp(maxDynGrDb, 0.0f, 48.0f);
    }

    double sampleRate = 48000.0;
    bool offlineRender = false;
    bool m_prepared = false;
    bool m_liveChainChecked = false;
    bool m_hasLiveChain = false;
    float m_lastGrDb = 0.0f;
    stoneridge::InputDspParams m_liveChainParams = MakeDefaultLiveChain();
    stoneridge::InputProcessor chain;
    stoneridge::DacConditioner safetyLimiter;
};

#include <sonore/plugin.h>

static const char* kRatioNames[] = { "4:1", "8:1", "British" };

static const sonore::ParamInfo kParams[SONORE_NUM_PARAMS] = {
    { "input", "Input", "dB", 0.0f, 1.0f, 0.0f, 0, "Input" },
    { "trim", "Trim", "dB", -12.0f, 12.0f, 0.0f, 0, "Input" },
    { "fet", "1176", "%", 0.0f, 1.0f, 0.0f, 0, "Dynamics" },
    { "ratio", "Ratio", "", 0.0f, 2.0f, 0.0f, 3, "Dynamics", kRatioNames, 3 },
    { "la2a", "LA-2A", "%", 0.0f, 1.0f, 0.0f, 0, "Dynamics" },
    { "beauty", "Beauty", "%", 0.0f, 1.0f, 0.0f, 0, "Tone" },
    { "low", "Low", "dB", -6.0f, 6.0f, 0.0f, 0, "Tone" },
    { "air", "Air", "dB", -6.0f, 8.0f, 0.0f, 0, "Tone" },
    { "preset", "Preset", "", 0.0f, 6.0f, 0.0f, 0, "Tone" },
};

static const sonore::PluginDescriptor kDesc = {
    "com.stoneridge.vocalchain",
    "Stoneridge Vocal Chain",
    "Stoneridge",
    "7.1.0",
    "Unified 192 kHz analog vocal chain shared with the Stoneridge ASIO driver.",
    "https://stoneridge.audio",
    false,
    kParams,
    SONORE_NUM_PARAMS,
    nullptr,
    0,
    "dynamics",
    nullptr,
    "support@stoneridge.audio",
    1,
    2,
    nullptr,
    0,
    false,
    false,
    sonore::EditorKind::Auto,
    { 960, 620, 1920, 1200, true },
    true,
    16,
};

#define SONORE_UI_WIDTH 1180
#define SONORE_UI_HEIGHT 740

#include <sonore/formats.h>
