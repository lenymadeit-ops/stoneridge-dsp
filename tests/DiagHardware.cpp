#include <catch2/catch_test_macros.hpp>
#include "engine/WasapiDeviceManager.h"
#include "engine/WasapiExclusiveStream.h"

#include <audioclient.h>
#include <setupapi.h>
#include <ks.h>
#include <ksmedia.h>
#include <iostream>
#include <iomanip>

#pragma comment(lib, "setupapi.lib")

TEST_CASE("Deep hardware probe for Mackie EM-91CU+", "[.hw]") {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        stoneridge::WasapiDeviceManager mgr;
        wil::com_ptr<IMMDevice> cap, ren;
        std::wstring name;
        bool isMackie = false;
        const bool found = mgr.FindMackieEndpoints(cap, ren, name, isMackie);
        std::wcout << L"[Probe] Found=" << found << L" isMackie=" << isMackie << L" Name=" << name << L"\n";
        REQUIRE(found);

        auto probeClient = [](const char* label, IMMDevice* dev) {
            if (!dev) return;
            wil::com_ptr<IAudioClient3> c3;
            HRESULT hr = dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(c3.put()));
            std::cout << "\n=== " << label << " (IAudioClient3 hr=0x" << std::hex << hr << std::dec << ") ===\n";
            if (FAILED(hr)) return;

            REFERENCE_TIME defP = 0, minP = 0;
            c3->GetDevicePeriod(&defP, &minP);
            std::cout << "  GetDevicePeriod: def=" << (defP / 10000.0) << " ms, min=" << (minP / 10000.0) << " ms\n";

            WAVEFORMATEX* mixFmt = nullptr;
            if (SUCCEEDED(c3->GetMixFormat(&mixFmt)) && mixFmt) {
                UINT32 defF = 0, fundF = 0, minF = 0, maxF = 0;
                if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mixFmt, &defF, &fundF, &minF, &maxF))) {
                    std::cout << "  IAudioClient3 Shared Engine Period (" << mixFmt->nSamplesPerSec << " Hz): "
                              << "min=" << minF << " (" << (minF * 1000.0 / mixFmt->nSamplesPerSec) << " ms), "
                              << "fund=" << fundF << ", def=" << defF << ", max=" << maxF << "\n";
                }
                CoTaskMemFree(mixFmt);
            }

            for (uint32_t rate : { 44100u, 48000u, 96000u }) {
                WAVEFORMATEXTENSIBLE fmt{};
                uint32_t bits = 0;
                if (!stoneridge::WasapiDeviceManager::ProbeBestExclusiveFormat(c3.get(), rate, fmt, bits)) {
                    std::cout << "  Rate " << rate << " Hz: NOT SUPPORTED in Exclusive Mode\n";
                    continue;
                }
                std::cout << "  Rate " << rate << " Hz: " << fmt.Format.nChannels << "ch "
                          << fmt.Format.wBitsPerSample << "-bit (valid=" << fmt.Samples.wValidBitsPerSample << ")\n";

                // Probe exact aligned Exclusive buffer frames for various requested frame counts
                for (uint32_t reqFrames : { 64u, 128u, 132u, 144u, 192u, 256u, 288u, 480u, 512u }) {
                    wil::com_ptr<IAudioClient> testClient;
                    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                             reinterpret_cast<void**>(testClient.put())))) continue;

                    REFERENCE_TIME reqP = static_cast<REFERENCE_TIME>((10000000.0 * reqFrames / rate) + 0.5);
                    if (reqP < minP) reqP = minP;

                    HRESULT ihr = testClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                         AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                         reqP, reqP,
                                                         reinterpret_cast<WAVEFORMATEX*>(&fmt), nullptr);
                    UINT32 actual = 0;
                    if (ihr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                        testClient->GetBufferSize(&actual);
                        std::cout << "    req=" << reqFrames << " -> NOT_ALIGNED -> aligned=" << actual
                                  << " (" << std::fixed << std::setprecision(2) << (actual * 1000.0 / rate) << " ms)\n";
                    } else if (SUCCEEDED(ihr)) {
                        testClient->GetBufferSize(&actual);
                        std::cout << "    req=" << reqFrames << " -> EXACT OK -> actual=" << actual
                                  << " (" << std::fixed << std::setprecision(2) << (actual * 1000.0 / rate) << " ms)\n";
                    } else {
                        std::cout << "    req=" << reqFrames << " -> FAILED hr=0x" << std::hex << ihr << std::dec << "\n";
                    }
                }
            }
        };

        probeClient("Render", ren.get());
        probeClient("Capture", cap.get());

        // Also check WDM-KS device interfaces for VID_0A73 (Mackie)
        std::cout << "\n=== WDM-KS Audio Filter Interfaces (Mackie VID_0A73) ===\n";
        HDEVINFO hDevInfo = SetupDiGetClassDevsW(&KSCATEGORY_AUDIO, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (hDevInfo != INVALID_HANDLE_VALUE) {
            SP_DEVICE_INTERFACE_DATA ifData{};
            ifData.cbSize = sizeof(ifData);
            for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, &KSCATEGORY_AUDIO, i, &ifData); ++i) {
                DWORD reqSize = 0;
                SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, nullptr, 0, &reqSize, nullptr);
                std::vector<BYTE> buf(reqSize);
                auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, detail, reqSize, nullptr, nullptr)) {
                    std::wstring path(detail->DevicePath);
                    if (path.find(L"vid_0a73") != std::wstring::npos || path.find(L"VID_0A73") != std::wstring::npos) {
                        std::wcout << L"  KS Path: " << path << L"\n";
                    }
                }
            }
            SetupDiDestroyDeviceInfoList(hDevInfo);
        }
    }
    CoUninitialize();
}
