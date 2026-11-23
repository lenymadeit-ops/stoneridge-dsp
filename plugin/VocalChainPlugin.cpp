// Stoneridge Vocal Chain. The same InputProcessor the ASIO driver prints.
#define SONORE_NUM_PARAMS 8

#include "dsp/InputProcessor.h"

#include <sonore/dsp.h>

struct SonoreDsp {
    void prepare(const sonore::ProcessSpec& spec) {
        sampleRate = spec.sampleRate > 1000.0 ? spec.sampleRate : 48000.0;
        chain.Prepare(sampleRate, 2);
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

        stoneridge::InputDspTelemetry tel{};
        if (right) chain.ProcessStereo(left, right, static_cast<uint32_t>(frames), params, tel);
        else chain.ProcessMono(left, static_cast<uint32_t>(frames), params, tel);
    }

    double sampleRate = 48000.0;
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
