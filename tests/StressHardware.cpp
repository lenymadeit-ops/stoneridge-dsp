#include <catch2/catch_test_macros.hpp>
#include "engine/WdmKsStream.h"
#include "engine/WasapiExclusiveStream.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

namespace {

// Simulates heavy FL Studio plugin DSP load for a specified fraction of the period budget
void SimulateHeavyPluginWork(double targetWorkUs, volatile float& sink) {
    LARGE_INTEGER freq{}, start{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    const double targetTicks = (targetWorkUs * static_cast<double>(freq.QuadPart)) / 1000000.0;

    float acc = sink;
    while (true) {
        for (int i = 0; i < 64; ++i) {
            acc = acc * 0.9999f + 0.0001f * std::sin(acc + static_cast<float>(i));
        }
        QueryPerformanceCounter(&now);
        if (static_cast<double>(now.QuadPart - start.QuadPart) >= targetTicks) {
            break;
        }
    }
    sink = acc;
}

struct StressContext {
    stoneridge::IAudioStreamBackend* stream = nullptr;
    uint32_t bufferSize = 128;
    double sampleRate = 48000.0;
    double pluginLoadFraction = 0.55;
    std::atomic<uint64_t> callbacks{0};
    double phase = 0.0;
    volatile float sink = 0.123f;
};

static StressContext* g_ctx = nullptr;

void StressCallback(long doubleBufIdx, ASIOBool) {
    auto* ctx = g_ctx;
    if (!ctx || !ctx->stream) return;

    float* outL = static_cast<float*>(ctx->stream->GetAsioBufferPtr(false, 0, static_cast<uint32_t>(doubleBufIdx)));
    float* outR = static_cast<float*>(ctx->stream->GetAsioBufferPtr(false, 1, static_cast<uint32_t>(doubleBufIdx)));

    // Generate quiet (-24 dBFS) continuous 440 Hz sine wave so test is audible & phase-clean
    const double phaseInc = 6.28318530717958647692 * 440.0 / ctx->sampleRate;
    for (uint32_t i = 0; i < ctx->bufferSize; ++i) {
        const float s = 0.06f * static_cast<float>(std::sin(ctx->phase));
        ctx->phase += phaseInc;
        if (ctx->phase > 6.28318530717958647692) ctx->phase -= 6.28318530717958647692;
        if (outL) outL[i] = s;
        if (outR) outR[i] = s;
    }

    // Burn realistic FL Studio multi-plugin CPU load inside the audio callback
    const double periodUs = (1000000.0 * ctx->bufferSize) / ctx->sampleRate;
    SimulateHeavyPluginWork(periodUs * ctx->pluginLoadFraction, ctx->sink);

    ctx->callbacks.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

TEST_CASE("Live EM-91CU+ WDM-KS Heavy-Load Stress Suite (64/128/256 @ 44.1/48/96kHz)", "[.stress]") {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Spawn 2 background CPU contention threads to simulate heavy Windows/DAW UI load
    std::atomic<bool> stopNoise{false};
    std::vector<std::thread> noiseThreads;
    for (int t = 0; t < 2; ++t) {
        noiseThreads.emplace_back([&stopNoise]() {
            volatile double x = 1.0;
            while (!stopNoise.load(std::memory_order_relaxed)) {
                x = std::sin(x + 0.01) + std::cos(x * 0.99);
            }
        });
    }

    struct Config {
        uint32_t rate;
        uint32_t buf;
        uint32_t irps;
        double   loadFrac;
        int      durationMs;
    };

    const Config configs[] = {
        { 44100,  64, 3, 0.50, 2000 }, // 1.45 ms buffer @ 44.1 kHz (beats ASIO4ALL!)
        { 44100, 128, 3, 0.65, 2500 }, // 2.90 ms buffer @ 44.1 kHz under 65% plugin load
        { 48000,  64, 3, 0.50, 2000 }, // 1.33 ms buffer @ 48 kHz
        { 48000, 128, 2, 0.55, 2500 }, // 2.67 ms buffer @ 48 kHz Ultra-Low 2x IRP
        { 96000, 128, 3, 0.50, 2000 }, // 1.33 ms buffer @ 96 kHz (16-bit native!)
        { 48000, 256, 2, 0.70, 2000 }, // 5.33 ms buffer @ 48 kHz under 70% plugin load
    };

    for (const auto& cfg : configs) {
        stoneridge::SharedState shm{};
        stoneridge::InitializeDefaultSharedState(shm);
        shm.ksIrpCount.store(cfg.irps, std::memory_order_relaxed);
        shm.gateEnabled.store(true, std::memory_order_relaxed);
        shm.audioHpfEnabled.store(true, std::memory_order_relaxed);
        shm.softClipEnabled.store(true, std::memory_order_relaxed);

        stoneridge::WdmKsStream stream;
        REQUIRE(stream.InitializeDevices(cfg.rate, cfg.buf, &shm));

        StressContext ctx{};
        ctx.stream             = &stream;
        ctx.bufferSize         = cfg.buf;
        ctx.sampleRate         = static_cast<double>(cfg.rate);
        ctx.pluginLoadFraction = cfg.loadFrac;
        g_ctx = &ctx;

        ASIOCallbacks cbs{};
        cbs.bufferSwitch = &StressCallback;

        const bool created = stream.CreateBuffers(cfg.buf, 0b11, 0b11, &cbs);
        if (!created) {
            std::cout << "[STRESS WDM-KS] CreateBuffers FAILED! Last KsCreatePin status=" << stream.GetLastPinStatus()
                      << " (0x" << std::hex << stream.GetLastPinStatus() << std::dec << ")\n";
        }
        REQUIRE(created);
        REQUIRE(stream.Start());

        Sleep(cfg.durationMs);
        stream.Stop();
        g_ctx = nullptr;

        const uint64_t count = ctx.callbacks.load();
        const double expected = (static_cast<double>(cfg.rate) * (cfg.durationMs / 1000.0)) / cfg.buf;
        const uint32_t xruns = shm.xrunCount.load();
        const uint32_t capUnder = stream.GetCaptureUnderruns();
        const uint32_t capTrims = stream.GetCaptureTrims();

        std::cout << std::fixed << std::setprecision(2)
                  << "[STRESS WDM-KS] " << cfg.rate << "Hz / " << cfg.buf << "smp (" << cfg.irps << "x IRP, "
                  << static_cast<int>(cfg.loadFrac * 100) << "% load): "
                  << "CBs=" << count << "/" << static_cast<int>(expected)
                  << " | In=" << shm.inputLatencyMs.load() << "ms Out=" << shm.outputLatencyMs.load() << "ms"
                  << " | MaxJit=" << shm.maxJitterUs.load() << "us"
                  << " | XRuns=" << xruns
                  << " | CapUnder=" << capUnder << " CapTrim=" << capTrims
                  << " | MicActive=" << shm.captureActive.load() << "\n";

        REQUIRE(shm.captureActive.load() == true);
        REQUIRE(count >= static_cast<uint64_t>(expected * 0.90));
        REQUIRE(xruns == 0);
        REQUIRE(capTrims == 0);

        stream.DisposeBuffers();
        stream.ShutdownDevices();
    }

    stopNoise.store(true, std::memory_order_relaxed);
    for (auto& th : noiseThreads) th.join();
    CoUninitialize();
}
