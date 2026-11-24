#include "dsp/DacConditioner.h"
#include "plugin/VocalChainPresets.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

TEST_CASE("v7 acoustic audit of six producer presets", "[v7][audit][presets]") {
    const double rates[] = { 44100.0, 48000.0, 96000.0 };
    double loudest = -120.0;
    double quietest = 0.0;
    for (double rate : rates) {
        for (int presetIndex = 0; presetIndex < 6; ++presetIndex) {
            stoneridge::InputProcessor proc;
            stoneridge::DacConditioner dac;
            proc.Prepare(rate, 1);
            dac.Prepare(rate);
            stoneridge::InputDspParams params{};
            stoneridge::ApplyVocalPreset(params, stoneridge::PresetByIndex(presetIndex));
            const uint32_t n = static_cast<uint32_t>(rate * 0.50);
            const uint32_t settle = static_cast<uint32_t>(rate * 0.30);
            std::vector<float> buf(n);
            for (uint32_t i = 0; i < n; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(rate);
                buf[i] = 0.12f * std::sin(2.0f * 3.14159265f * 100.0f * t)
                       + 0.04f * std::sin(2.0f * 3.14159265f * 200.0f * t);
            }
            stoneridge::InputDspTelemetry tel{};
            proc.ProcessMono(buf.data(), n, params, tel);
            stoneridge::DacTelemetry dacTel{};
            dac.ProcessStereoFloat(buf.data(), nullptr, buf.data(), nullptr, n, true, dacTel);
            double sum = 0.0;
            double energy = 0.0;
            float peak = 0.0f;
            bool finite = true;
            uint32_t counted = 0;
            for (uint32_t i = 0; i < n; ++i) {
                const float s = buf[i];
                if (!std::isfinite(s)) finite = false;
                peak = std::max(peak, std::fabs(s));
                if (i >= settle) {
                    sum += static_cast<double>(s);
                    energy += static_cast<double>(s) * s;
                    ++counted;
                }
            }
            const double dc = std::fabs(sum / static_cast<double>(std::max(1u, counted)));
            const double rms = 20.0 * std::log10(std::max(std::sqrt(energy / static_cast<double>(std::max(1u, counted))), 1.0e-12));
            loudest = std::max(loudest, rms);
            quietest = std::min(quietest, rms);
            INFO("preset " << presetIndex << " rate " << rate << " dc " << dc << " peak " << peak << " rms " << rms);
            REQUIRE(finite);
            REQUIRE(peak <= 0.9661f);
            REQUIRE(dc < 3.162e-4);
        }
    }
    REQUIRE((loudest - quietest) <= 6.5);
}
