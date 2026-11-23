// Stoneridge Vocal Chain plugin face. The DSP lives in the shared vocal headers.
#define SONORE_NUM_PARAMS 8

#include <sonore/dsp.h>

struct SonoreDsp {
    void prepare(const sonore::ProcessSpec& spec) { sampleRate = spec.sampleRate; }
    void process(sonore::AudioBlock<float>& block, const float* params) {
        const int channels = static_cast<int>(block.getNumChannels());
        const int frames = static_cast<int>(block.getNumSamples());
        const float drive = params ? params[0] : 0.0f;
        const float trim = params ? params[1] : 0.0f;
        const float gain = std::pow(10.0f, (trim + drive * 6.0f) / 20.0f);
        for (int c = 0; c < channels; ++c) {
            float* data = block.getChannelPointer(static_cast<size_t>(c));
            if (!data) continue;
            for (int i = 0; i < frames; ++i) {
                const float x = data[i] * gain;
                data[i] = x / (1.0f + 0.15f * drive * std::fabs(x));
            }
        }
        (void) sampleRate;
    }
    double sampleRate = 48000.0;
};

#include <sonore/plugin.h>

static const char* kRatioNames[] = { "4:1", "8:1", "British" };

static const sonore::ParamInfo kParams[SONORE_NUM_PARAMS] = {
    { "input", "Input", "dB", 0.0f, 1.0f, 0.2f, 0, "Input" },
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
