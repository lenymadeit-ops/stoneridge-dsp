#include <catch2/catch_test_macros.hpp>
#include "engine/WdmKsHardwareProbe.h"
#include "engine/MackieHidMonitor.h"
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <ks.h>
#include <ksmedia.h>
#include <iostream>
#include <vector>
#include <string>

#pragma comment(lib, "setupapi.lib")

namespace {

bool KsProperty(HANDLE h, const GUID& set, ULONG id, ULONG flags,
                void* inExtra, DWORD inExtraSize, void* outBuf, DWORD outSize, DWORD* bytesRet) {
    std::vector<BYTE> propBuf(sizeof(KSPROPERTY) + inExtraSize);
    auto* prop = reinterpret_cast<KSPROPERTY*>(propBuf.data());
    prop->Set   = set;
    prop->Id    = id;
    prop->Flags = flags;
    if (inExtra && inExtraSize > 0) {
        std::memcpy(propBuf.data() + sizeof(KSPROPERTY), inExtra, inExtraSize);
    }
    DWORD ret = 0;
    const BOOL ok = DeviceIoControl(h, IOCTL_KS_PROPERTY,
                                    propBuf.data(), static_cast<DWORD>(propBuf.size()),
                                    outBuf, outSize, &ret, nullptr);
    if (bytesRet) *bytesRet = ret;
    return ok != FALSE;
}

} // namespace

TEST_CASE("Probe WDM-KS pins on Mackie EM-91CU+", "[.ks]") {
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&KSCATEGORY_AUDIO, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    REQUIRE(hDevInfo != INVALID_HANDLE_VALUE);

    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(ifData);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, &KSCATEGORY_AUDIO, i, &ifData); ++i) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, nullptr, 0, &reqSize, nullptr);
        std::vector<BYTE> buf(reqSize);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, detail, reqSize, nullptr, nullptr)) continue;

        std::wstring path(detail->DevicePath);
        if (path.find(L"vid_0a73") == std::wstring::npos && path.find(L"VID_0A73") == std::wstring::npos) continue;

        std::wcout << L"[KS] Opening: " << path << L"\n";
        HANDLE hFilter = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (hFilter == INVALID_HANDLE_VALUE) {
            std::cout << "[KS] CreateFile failed err=" << GetLastError() << "\n";
            continue;
        }

        ULONG pinCount = 0;
        DWORD bytes = 0;
        if (KsProperty(hFilter, KSPROPSETID_Pin, KSPROPERTY_PIN_CTYPES, KSPROPERTY_TYPE_GET,
                       nullptr, 0, &pinCount, sizeof(pinCount), &bytes)) {
            std::cout << "[KS] Pin count = " << pinCount << "\n";
            for (ULONG pinId = 0; pinId < pinCount; ++pinId) {
                KSPIN_DATAFLOW dataflow{};
                KSPIN_COMMUNICATION comm{};
                ULONG extra[2] = { pinId, 0 }; // KSP_PIN has PinId + Reserved after KSPROPERTY
                KsProperty(hFilter, KSPROPSETID_Pin, KSPROPERTY_PIN_DATAFLOW, KSPROPERTY_TYPE_GET,
                           extra, sizeof(extra), &dataflow, sizeof(dataflow), &bytes);
                KsProperty(hFilter, KSPROPSETID_Pin, KSPROPERTY_PIN_COMMUNICATION, KSPROPERTY_TYPE_GET,
                           extra, sizeof(extra), &comm, sizeof(comm), &bytes);

                std::cout << "  Pin " << pinId
                          << " flow=" << (dataflow == KSPIN_DATAFLOW_IN ? "RENDER(IN)" : "CAPTURE(OUT)")
                          << " comm=" << comm << "\n";

                // Query dataranges
                DWORD rangeBytes = 0;
                KsProperty(hFilter, KSPROPSETID_Pin, KSPROPERTY_PIN_DATARANGES, KSPROPERTY_TYPE_GET,
                           extra, sizeof(extra), nullptr, 0, &rangeBytes);
                if (rangeBytes > sizeof(KSMULTIPLE_ITEM)) {
                    std::vector<BYTE> rbuf(rangeBytes);
                    if (KsProperty(hFilter, KSPROPSETID_Pin, KSPROPERTY_PIN_DATARANGES, KSPROPERTY_TYPE_GET,
                                   extra, sizeof(extra), rbuf.data(), rangeBytes, &rangeBytes)) {
                        auto* multi = reinterpret_cast<KSMULTIPLE_ITEM*>(rbuf.data());
                        BYTE* ptr = rbuf.data() + sizeof(KSMULTIPLE_ITEM);
                        for (ULONG r = 0; r < multi->Count; ++r) {
                            auto* dr = reinterpret_cast<KSDATARANGE*>(ptr);
                            if (dr->MajorFormat == KSDATAFORMAT_TYPE_AUDIO &&
                                dr->SubFormat == KSDATAFORMAT_SUBTYPE_PCM &&
                                dr->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
                                auto* aud = reinterpret_cast<KSDATARANGE_AUDIO*>(dr);
                                std::cout << "    PCM: ch=" << aud->MaximumChannels
                                          << " bits=" << aud->MinimumBitsPerSample << "-" << aud->MaximumBitsPerSample
                                          << " rate=" << aud->MinimumSampleFrequency << "-" << aud->MaximumSampleFrequency << "\n";
                            }
                            ptr += (dr->FormatSize + 7) & ~7;
                        }
                    }
                }
            }
        }
        CloseHandle(hFilter);

        // Probe Hardware Topology, Feature Unit PGA/DAC Gains, Mono Support, WaveRT & HID MI_03
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            stoneridge::WdmKsHardwareController hw;
            hw.Initialize(path, 1, 0);
            const auto& caps = hw.GetCapabilities();
            std::cout << "[HW Probe] TopologyNodes=" << caps.topologyNodeCount
                      << " DirectKsNodes=" << caps.usingDirectKsNodes
                      << " (ADC vol=" << caps.adcVolNode << " mute=" << caps.adcMuteNode
                      << ", DAC vol=" << caps.dacVolNode << " mute=" << caps.dacMuteNode << ")"
                      << " MonoCap=" << caps.captureSupportsMono
                      << " WaveRT=" << caps.supportsWaveRt
                      << " Sidetone=" << caps.hasSidetoneNode << "\n";
            std::cout << "[HW Probe] InputPGA=" << caps.hasInputPgaGain
                      << " [" << caps.inputPgaMinDb << " .. " << caps.inputPgaMaxDb
                      << " dB, step=" << caps.inputPgaStepDb << " dB, cur=" << caps.inputPgaCurrentDb << " dB]\n";
            std::cout << "[HW Probe] OutputDAC=" << caps.hasOutputDacGain
                      << " [" << caps.outputDacMinDb << " .. " << caps.outputDacMaxDb
                      << " dB, step=" << caps.outputDacStepDb << " dB, cur=" << caps.outputDacCurrentDb
                      << " dB, bal=" << caps.outputDacBalanceDb << " dB]\n";

            stoneridge::SharedState shm{};
            stoneridge::MackieHidMonitor hid;
            hid.Start(&shm);
            Sleep(60);
            std::cout << "[HID Probe] Opened MI_03 Consumer HID interfaces=" << hid.GetDeviceCount() << "\n";
            hid.Stop();
        }
        CoUninitialize();
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
}
