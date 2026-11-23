#include "asio/asio_abi.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

// {B8E3A1D4-7C2F-4E9A-9D12-8F4E2C1A5B01}
constexpr GUID kClsidStoneridge = {
    0xb8e3a1d4, 0x7c2f, 0x4e9a, { 0x9d, 0x12, 0x8f, 0x4e, 0x2c, 0x1a, 0x5b, 0x01 }
};

// {232685C6-6548-49D8-846D-4141A3EF7560}
constexpr GUID kClsidAsio4All = {
    0x232685c6, 0x6548, 0x49d8, { 0x84, 0x6d, 0x41, 0x41, 0xa3, 0xef, 0x75, 0x60 }
};

struct BenchState {
    std::vector<ASIOBufferInfo> bufs;
    ASIOSampleType sampleType = ASIOSTFloat32LSB;
    long   bufferSize = 128;
    double sampleRate = 48000.0;
    double loadFraction = 0.50;
    uint64_t qpcFreq = 1;
    LARGE_INTEGER prevQpc{};
    bool hasPrev = false;
    std::vector<double> jittersUs;
    uint32_t lateCallbacks = 0;
    uint64_t callbacks = 0;
    volatile float sink = 0.25f;
};

BenchState* g_bs = nullptr;

void BurnPluginWork(double targetUs, uint64_t qpcFreq, volatile float& sink) {
    LARGE_INTEGER start{}, now{};
    QueryPerformanceCounter(&start);
    const double targetTicks = (targetUs * static_cast<double>(qpcFreq)) / 1000000.0;
    float acc = sink;
    while (true) {
        for (int i = 0; i < 48; ++i) {
            acc = acc * 0.9999f + 0.0001f * std::sin(acc + static_cast<float>(i));
        }
        QueryPerformanceCounter(&now);
        if (static_cast<double>(now.QuadPart - start.QuadPart) >= targetTicks) break;
    }
    sink = acc;
}

void BenchSwitch(long idx, ASIOBool) {
    auto* bs = g_bs;
    if (!bs) return;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const double periodUs = (1000000.0 * bs->bufferSize) / bs->sampleRate;

    if (bs->hasPrev) {
        const double dtUs = (static_cast<double>(now.QuadPart - bs->prevQpc.QuadPart) * 1000000.0) / bs->qpcFreq;
        const double jitUs = std::fabs(dtUs - periodUs);
        if (bs->callbacks > 4 && bs->jittersUs.size() < 4096) {
            bs->jittersUs.push_back(jitUs);
            if (dtUs > periodUs * 1.85) {
                ++bs->lateCallbacks;
            }
        }
    }
    bs->prevQpc = now;
    bs->hasPrev = true;

    // Zero output buffers safely according to driver sample type
    const size_t sampleBytes = (bs->sampleType == ASIOSTInt16LSB || bs->sampleType == ASIOSTInt16MSB) ? 2 :
                               (bs->sampleType == ASIOSTInt24LSB || bs->sampleType == ASIOSTInt24MSB) ? 3 : 4;
    for (const auto& b : bs->bufs) {
        if (!b.isInput && b.buffers[idx]) {
            std::memset(b.buffers[idx], 0, static_cast<size_t>(bs->bufferSize) * sampleBytes);
        }
    }

    BurnPluginWork(periodUs * bs->loadFraction, bs->qpcFreq, bs->sink);
    ++bs->callbacks;
}

ASIOTime* BenchSwitchTimeInfo(ASIOTime* t, long idx, ASIOBool direct) {
    BenchSwitch(idx, direct);
    return t;
}

void BenchRateChange(ASIOSampleRate) {}

long BenchMessage(long selector, long value, void*, double*) {
    if (selector == kAsioSelectorSupported) {
        return (value == kAsioEngineVersion || value == kAsioSupportsTimeInfo || value == kAsioResetRequest) ? 1 : 0;
    }
    if (selector == kAsioEngineVersion) return 2;
    if (selector == kAsioSupportsTimeInfo) return 1;
    return 0;
}

struct DriverResult {
    bool   ok = false;
    std::string name;
    long   actualBuf = 0;
    double inMs = 0.0;
    double outMs = 0.0;
    double p99JitterUs = 0.0;
    int    maxCleanLoadPct = 0;
};

DriverResult RunDriverShootout(const char* label, const GUID& clsid, double targetRate) {
    DriverResult res{};
    res.name = label;

    IASIO* asio = nullptr;
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&asio))) || !asio) {
        std::cout << "[Shootout] " << label << ": CoCreateInstance failed (not installed or busy)\n";
        return res;
    }

    if (asio->init(GetDesktopWindow()) != ASIOTrue) {
        std::cout << "[Shootout] " << label << ": IASIO::init failed\n";
        asio->Release();
        return res;
    }

    if (asio->canSampleRate(targetRate) == ASE_OK) {
        asio->setSampleRate(targetRate);
    }
    ASIOSampleRate rate = targetRate;
    asio->getSampleRate(&rate);

    long inCh = 0, outCh = 0;
    asio->getChannels(&inCh, &outCh);
    long minS = 0, maxS = 0, prefS = 0, gran = 0;
    asio->getBufferSize(&minS, &maxS, &prefS, &gran);
    if (outCh < 2 || prefS <= 0) {
        std::cout << "[Shootout] " << label << ": invalid channels/buffer (in=" << inCh << " out=" << outCh << " pref=" << prefS << ")\n";
        asio->Release();
        return res;
    }

    ASIOChannelInfo ci{};
    ci.channel = 0;
    ci.isInput = ASIOFalse;
    asio->getChannelInfo(&ci);

    res.actualBuf = prefS;

    ASIOCallbacks cbs{};
    cbs.bufferSwitch         = &BenchSwitch;
    cbs.bufferSwitchTimeInfo = &BenchSwitchTimeInfo;
    cbs.sampleRateDidChange  = &BenchRateChange;
    cbs.asioMessage          = &BenchMessage;

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);

    // Ramp simulated FL Studio plugin load: 40%, 55%, 70%, 85%
    for (int loadPct : { 40, 55, 70, 85 }) {
        BenchState bs{};
        bs.sampleType   = ci.type;
        bs.bufferSize   = prefS;
        bs.sampleRate   = rate;
        bs.loadFraction = loadPct / 100.0;
        bs.qpcFreq      = static_cast<uint64_t>(freq.QuadPart);

        const long useIn = std::min(inCh, 2L);
        bs.bufs.assign(static_cast<size_t>(useIn + 2), ASIOBufferInfo{});
        long idx = 0;
        for (long c = 0; c < useIn; ++c) {
            bs.bufs[idx].isInput = ASIOTrue;
            bs.bufs[idx].channelNum = c;
            ++idx;
        }
        bs.bufs[idx].isInput = ASIOFalse; bs.bufs[idx].channelNum = 0; ++idx;
        bs.bufs[idx].isInput = ASIOFalse; bs.bufs[idx].channelNum = 1; ++idx;

        g_bs = &bs;
        if (asio->createBuffers(bs.bufs.data(), static_cast<long>(bs.bufs.size()), prefS, &cbs) != ASE_OK) {
            g_bs = nullptr;
            break;
        }

        long inLat = 0, outLat = 0;
        asio->getLatencies(&inLat, &outLat);
        res.inMs  = (inLat * 1000.0) / rate;
        res.outMs = (outLat * 1000.0) / rate;

        if (asio->start() != ASE_OK) {
            asio->disposeBuffers();
            g_bs = nullptr;
            break;
        }

        Sleep(1200);
        asio->stop();
        asio->disposeBuffers();
        g_bs = nullptr;

        double p99 = 0.0;
        if (!bs.jittersUs.empty()) {
            std::sort(bs.jittersUs.begin(), bs.jittersUs.end());
            p99 = bs.jittersUs[static_cast<size_t>(bs.jittersUs.size() * 0.99)];
        }
        res.ok = true;
        if (loadPct == 55 || res.p99JitterUs == 0.0) {
            res.p99JitterUs = p99;
        }
        if (bs.lateCallbacks == 0 && bs.callbacks >= 20) {
            res.maxCleanLoadPct = loadPct;
        }
        std::cout << "  [" << label << "] load=" << loadPct << "% | CBs=" << bs.callbacks
                  << " | Late=" << bs.lateCallbacks << " | P99Jit=" << std::fixed << std::setprecision(1) << p99 << " us\n";
    }

    asio->Release();
    return res;
}

} // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::cout << "====================================================================\n";
    std::cout << "  HEAD-TO-HEAD COM ASIO SHOOTOUT: STONERIDGE ASIO vs. ASIO4ALL v2\n";
    std::cout << "====================================================================\n";

    const auto sr = RunDriverShootout("Stoneridge ASIO (WDM-KS)", kClsidStoneridge, 48000.0);
    Sleep(300);
    const auto a4 = RunDriverShootout("ASIO4ALL v2", kClsidAsio4All, 48000.0);

    std::cout << "\n| Driver | Buffer | In Lat | Out Lat | Total RTL | P99 Jitter | Max Clean Load |\n";
    std::cout << "|---|---:|---:|---:|---:|---:|---:|\n";
    auto printRow = [](const DriverResult& r) {
        if (!r.ok) {
            std::cout << "| " << r.name << " | N/A | N/A | N/A | N/A | N/A | N/A |\n";
            return;
        }
        std::cout << std::fixed << std::setprecision(2)
                  << "| " << r.name
                  << " | " << r.actualBuf
                  << " | " << r.inMs << " ms"
                  << " | " << r.outMs << " ms"
                  << " | " << (r.inMs + r.outMs) << " ms"
                  << " | " << std::setprecision(0) << r.p99JitterUs << " us"
                  << " | " << r.maxCleanLoadPct << "% |\n";
    };
    printRow(sr);
    printRow(a4);

    CoUninitialize();
    return sr.ok ? 0 : 1;
}
