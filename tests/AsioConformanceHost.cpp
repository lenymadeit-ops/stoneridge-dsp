#include "asio/asio_abi.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

// {B8E3A1D4-7C2F-4E9A-9D12-8F4E2C1A5B01}
constexpr GUID kClsidStoneridge = {
    0xb8e3a1d4, 0x7c2f, 0x4e9a, { 0x9d, 0x12, 0x8f, 0x4e, 0x2c, 0x1a, 0x5b, 0x01 }
};

std::atomic<uint64_t> g_cbCount{0};
std::atomic<uint64_t> g_resetRequests{0};
std::vector<ASIOBufferInfo> g_bufs;
long g_bufSize = 128;

void OnBufferSwitch(long idx, ASIOBool) {
    for (const auto& b : g_bufs) {
        if (!b.isInput && b.buffers[idx]) {
            std::memset(b.buffers[idx], 0, static_cast<size_t>(g_bufSize) * sizeof(float));
        }
    }
    g_cbCount.fetch_add(1, std::memory_order_relaxed);
}

ASIOTime* OnBufferSwitchTimeInfo(ASIOTime* t, long idx, ASIOBool direct) {
    OnBufferSwitch(idx, direct);
    return t;
}

void OnSampleRateChange(ASIOSampleRate) {}

long OnAsioMessage(long selector, long value, void*, double*) {
    switch (selector) {
        case kAsioSelectorSupported:
            if (value == kAsioResetRequest || value == kAsioEngineVersion ||
                value == kAsioSupportsTimeInfo || value == kAsioOverload) {
                return 1;
            }
            return 0;
        case kAsioEngineVersion:
            return 2;
        case kAsioSupportsTimeInfo:
            return 1;
        case kAsioResetRequest:
            g_resetRequests.fetch_add(1, std::memory_order_relaxed);
            return 1;
        default:
            return 0;
    }
}

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[FAIL] " << msg << "\n"; \
            return 1; \
        } \
    } while (0)

} // namespace

int main() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CHECK(SUCCEEDED(hr), "CoInitializeEx failed");

    IASIO* asio = nullptr;
    hr = CoCreateInstance(kClsidStoneridge, nullptr, CLSCTX_INPROC_SERVER, kClsidStoneridge,
                          reinterpret_cast<void**>(&asio));
    CHECK(SUCCEEDED(hr) && asio != nullptr, "CoCreateInstance(StoneridgeASIO) failed - is DLL registered?");

    char drvName[64]{};
    asio->getDriverName(drvName);
    std::cout << "[COM] Loaded Driver: " << drvName << " (v" << asio->getDriverVersion() << ")\n";

    CHECK(asio->init(GetDesktopWindow()) == ASIOTrue, "IASIO::init failed");

    long inCh = 0, outCh = 0;
    CHECK(asio->getChannels(&inCh, &outCh) == ASE_OK, "getChannels failed");
    CHECK(outCh >= 2, "Expected >= 2 output channels");
    std::cout << "[COM] Channels: In=" << inCh << " Out=" << outCh << "\n";

    long minS = 0, maxS = 0, prefS = 0, gran = 0;
    CHECK(asio->getBufferSize(&minS, &maxS, &prefS, &gran) == ASE_OK, "getBufferSize failed");
    std::cout << "[COM] BufferSize: min=" << minS << " max=" << maxS << " pref=" << prefS << " gran=" << gran << "\n";

    // Channel info check
    for (long c = 0; c < outCh; ++c) {
        ASIOChannelInfo ci{};
        ci.channel = c;
        ci.isInput = ASIOFalse;
        CHECK(asio->getChannelInfo(&ci) == ASE_OK, "getChannelInfo(out) failed");
    }

    // DAW Quirk 1: stop() before start() and disposeBuffers() before createBuffers() must not crash
    CHECK(asio->stop() == ASE_OK, "stop() before start() failed");
    CHECK(asio->disposeBuffers() == ASE_OK, "disposeBuffers() before createBuffers() failed");

    // DAW Quirk 2: Sample-rate switching authority (44.1 -> 48 -> 96 -> 44.1 kHz)
    for (double rate : { 44100.0, 48000.0, 96000.0, 44100.0 }) {
        if (asio->canSampleRate(rate) == ASE_OK) {
            CHECK(asio->setSampleRate(rate) == ASE_OK, "setSampleRate failed");
            ASIOSampleRate actual = 0.0;
            CHECK(asio->getSampleRate(&actual) == ASE_OK, "getSampleRate failed");
            CHECK(std::fabs(actual - rate) < 1.0, "getSampleRate mismatch after setSampleRate");
            std::cout << "[COM] Rate switch OK: " << rate << " Hz\n";
        }
    }

    ASIOCallbacks cbs{};
    cbs.bufferSwitch         = &OnBufferSwitch;
    cbs.bufferSwitchTimeInfo = &OnBufferSwitchTimeInfo;
    cbs.sampleRateDidChange  = &OnSampleRateChange;
    cbs.asioMessage          = &OnAsioMessage;

    // DAW Quirk 3: Output-only buffers (like FL Studio when no mic track is armed)
    {
        g_bufSize = prefS;
        g_bufs.assign(2, ASIOBufferInfo{});
        g_bufs[0].isInput = ASIOFalse; g_bufs[0].channelNum = 0;
        g_bufs[1].isInput = ASIOFalse; g_bufs[1].channelNum = 1;
        g_cbCount.store(0);
        CHECK(asio->createBuffers(g_bufs.data(), 2, g_bufSize, &cbs) == ASE_OK, "createBuffers(out-only) failed");
        CHECK(asio->start() == ASE_OK, "start(out-only) failed");
        const DWORD outWaitMs = std::max<DWORD>(150, static_cast<DWORD>((15.0 * g_bufSize * 1000.0) / 44100.0));
        Sleep(outWaitMs);
        CHECK(asio->stop() == ASE_OK, "stop(out-only) failed");
        CHECK(asio->disposeBuffers() == ASE_OK, "disposeBuffers(out-only) failed");
        CHECK(g_cbCount.load() >= 10, "Output-only callbacks too low");
        std::cout << "[COM] Output-only mode OK (" << g_cbCount.load() << " callbacks in " << outWaitMs << "ms)\n";
    }

    // DAW Quirk 4: 12x Rapid Full-Duplex Start/Stop Endurance (checks handle leaks & clean teardown)
    for (int cycle = 0; cycle < 12; ++cycle) {
        g_bufSize = (cycle % 2 == 0) ? 128 : 64;
        const long totalCh = std::min(inCh, 2L) + 2;
        g_bufs.assign(static_cast<size_t>(totalCh), ASIOBufferInfo{});
        long idx = 0;
        for (long c = 0; c < std::min(inCh, 2L); ++c) {
            g_bufs[idx].isInput = ASIOTrue;
            g_bufs[idx].channelNum = c;
            ++idx;
        }
        g_bufs[idx].isInput = ASIOFalse; g_bufs[idx].channelNum = 0; ++idx;
        g_bufs[idx].isInput = ASIOFalse; g_bufs[idx].channelNum = 1; ++idx;

        g_cbCount.store(0);
        CHECK(asio->createBuffers(g_bufs.data(), totalCh, g_bufSize, &cbs) == ASE_OK, "createBuffers(endurance) failed");

        long inLat = 0, outLat = 0;
        CHECK(asio->getLatencies(&inLat, &outLat) == ASE_OK, "getLatencies failed");
        CHECK(asio->start() == ASE_OK, "start(endurance) failed");
        Sleep(80);
        // Deliberately test disposeBuffers() directly without explicit stop() on odd cycles!
        if (cycle % 2 == 0) {
            CHECK(asio->stop() == ASE_OK, "stop(endurance) failed");
        }
        CHECK(asio->disposeBuffers() == ASE_OK, "disposeBuffers(endurance) failed");
        CHECK(g_cbCount.load() >= 5, "Endurance cycle callbacks too low");
    }
    std::cout << "[COM] 12x Rapid Full-Duplex Start/Stop Endurance PASSED!\n";

    asio->Release();
    CoUninitialize();
    std::cout << "[COM] ALL ASIO2 CONFORMANCE CHECKS PASSED!\n";
    return 0;
}
