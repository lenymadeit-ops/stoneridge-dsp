// Stoneridge Vocal Chain. The same InputProcessor the ASIO driver prints.
#define SONORE_NUM_PARAMS 9

#include "dsp/InputProcessor.h"
#include "plugin/VocalChainPresets.h"

#include <sonore/dsp.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>

struct SonoreDsp {
    void prepare(const sonore::ProcessSpec& spec) {
        sampleRate = spec.sampleRate > 1000.0 ? spec.sampleRate : 48000.0;
        chain.Prepare(sampleRate, 2);
        liveChain.clear();
        if (const char* app = std::getenv("APPDATA")) {
            const std::string path = std::string(app) + "\\StoneridgeDSP\\live_tracking_chain.json";
            std::ifstream in(path);
            if (in) {
                std::string line;
                while (std::getline(in, line)) liveChain += line;
            }
        }
    }

    static float JsonFloat(const std::string& json, const char* key, float fallback) {
        const std::string token = std::string("\"") + key + "\"";
        const auto pos = json.find(token);
        if (pos == std::string::npos) return fallback;
        const auto colon = json.find(':', pos + token.size());
        if (colon == std::string::npos) return fallback;
        return std::strtof(json.c_str() + colon + 1, nullptr);
    }

    void process(sonore::AudioBlock<float>& block, const float* p) {
        const int channels = static_cast<int>(block.getNumChannels());
        const int frames = static_cast<int>(block.getNumSamples());
        if (frames <= 0 || channels <= 0) return;
        float* left = block.getChannelPointer(0);
        if (!left) return;
        float* right = channels > 1 ? block.getChannelPointer(1) : nullptr;

        stoneridge::InputDspParams params{};
        params.softClipEnabled = true;
        params.molecularClip = true;
        params.wdfEqEnabled = true;
        const float presetValue = p ? p[8] : 0.0f;
        const int presetIndex = static_cast<int>(presetValue + 0.001f);
        if (presetIndex >= 1 && presetIndex <= 6) {
            stoneridge::ApplyVocalPreset(params, stoneridge::PresetByIndex(presetIndex - 1));
        } else if (presetValue >= 0.5f && presetValue < 1.0f && !liveChain.empty()) {
            params.gainDb = JsonFloat(liveChain, "inputGainDb", 0.0f);
            params.preampDriveDb = JsonFloat(liveChain, "preampDriveDb", 0.0f);
            params.preampModel = static_cast<uint32_t>(JsonFloat(liveChain, "preampModel", 0.0f));
            params.fetPeakCatchAmount = JsonFloat(liveChain, "fetPeakCatchAmount", 0.0f);
            params.fetRatioMode = static_cast<uint32_t>(JsonFloat(liveChain, "fetRatioMode", 0.0f));
            params.vocalLevelerAmount = JsonFloat(liveChain, "vocalLevelerAmount", 0.0f);
            params.beautyAmount = JsonFloat(liveChain, "beautyAmount", 0.0f);
            params.consoleLowDb = JsonFloat(liveChain, "consoleLowDb", 0.0f);
            params.consoleAirDb = JsonFloat(liveChain, "consoleAirDb", 0.0f);
            params.consoleEqModel = static_cast<uint32_t>(JsonFloat(liveChain, "consoleEqModel", 0.0f));
            params.micModel = static_cast<uint32_t>(JsonFloat(liveChain, "micModel", 0.0f));
        } else {
            params.gainDb = p ? p[1] : 0.0f;
            params.preampDriveDb = p ? p[0] * 12.0f : 0.0f;
            params.preampModel = (params.preampDriveDb > 0.05f) ? 1u : 0u;
            params.fetPeakCatchAmount = p ? p[2] : 0.0f;
            params.fetRatioMode = p ? static_cast<uint32_t>(p[3]) : 0u;
            params.vocalLevelerAmount = p ? p[4] : 0.0f;
            params.beautyAmount = p ? p[5] : 0.0f;
            params.consoleLowDb = p ? p[6] : 0.0f;
            params.consoleAirDb = p ? p[7] : 0.0f;
            params.consoleEqModel = (std::fabs(params.consoleLowDb) > 0.05f || std::fabs(params.consoleAirDb) > 0.05f) ? 1u : 0u;
        }

        stoneridge::InputDspTelemetry tel{};
        if (right) chain.ProcessStereo(left, right, static_cast<uint32_t>(frames), params, tel);
        else chain.ProcessMono(left, static_cast<uint32_t>(frames), params, tel);

        auto ceiling = [](float x) noexcept {
            const float a = std::fabs(x);
            if (a <= 0.90f) return x;
            const float sign = (x < 0.0f) ? -1.0f : 1.0f;
            const float over = (a - 0.90f) / 0.066f;
            const float shaped = 0.90f + 0.066f * std::tanh(over);
            return sign * shaped;
        };
        for (int i = 0; i < frames; ++i) {
            left[i] = ceiling(left[i]);
            if (right) right[i] = ceiling(right[i]);
        }
    }

    double sampleRate = 48000.0;
    std::string liveChain;
    stoneridge::InputProcessor chain;
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
    "7.0.0",
    "Tracking vocal chain shared with the Stoneridge ASIO driver.",
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
