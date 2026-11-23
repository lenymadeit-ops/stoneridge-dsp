#include "engine/MackieHidMonitor.h"

#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <string>
#include <vector>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

namespace stoneridge {

MackieHidMonitor::~MackieHidMonitor() {
    Stop();
}

bool MackieHidMonitor::Start(SharedState* sharedState, MuteCallback onHardwareMute, VolumeStepCallback onVolumeStep) {
    Stop();
    m_sharedState  = sharedState;
    m_onMute       = std::move(onHardwareMute);
    m_onVolumeStep = std::move(onVolumeStep);

    m_stopEvent.create(wil::EventOptions::ManualReset);
    if (!m_stopEvent.is_valid()) {
        return false;
    }

    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&MackieHidMonitor::MonitorLoop, this);
    return true;
}

void MackieHidMonitor::Stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (m_stopEvent.is_valid()) {
        m_stopEvent.SetEvent();
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void MackieHidMonitor::MonitorLoop() {
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return;
    }

    struct HidEndpoint {
        wil::unique_handle   handle;
        wil::unique_event    evt;
        OVERLAPPED           ov{};
        PHIDP_PREPARSED_DATA ppd = nullptr;
        std::vector<BYTE>    buf;
        bool                 pending = false;
        bool                 prevMuteBtn = false;
        bool                 prevVolUp   = false;
        bool                 prevVolDown = false;

        ~HidEndpoint() {
            if (ppd) {
                HidD_FreePreparsedData(ppd);
                ppd = nullptr;
            }
        }
        HidEndpoint() = default;
        HidEndpoint(const HidEndpoint&) = delete;
        HidEndpoint& operator=(const HidEndpoint&) = delete;
        HidEndpoint(HidEndpoint&& other) noexcept
            : handle(std::move(other.handle)),
              evt(std::move(other.evt)),
              ov(other.ov),
              ppd(other.ppd),
              buf(std::move(other.buf)),
              pending(other.pending),
              prevMuteBtn(other.prevMuteBtn),
              prevVolUp(other.prevVolUp),
              prevVolDown(other.prevVolDown) {
            other.ppd = nullptr;
        }
        HidEndpoint& operator=(HidEndpoint&&) = delete;
    };

    std::vector<HidEndpoint> endpoints;
    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(ifData);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, i, &ifData); ++i) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &reqSize, nullptr);
        if (reqSize == 0) continue;

        std::vector<BYTE> detailBuf(reqSize);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, reqSize, nullptr, nullptr)) {
            continue;
        }

        std::wstring path(detail->DevicePath);
        if (path.find(L"vid_0a73") == std::wstring::npos && path.find(L"VID_0A73") == std::wstring::npos) {
            continue;
        }

        wil::unique_handle h(CreateFileW(path.c_str(), GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                         OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (!h) continue;

        PHIDP_PREPARSED_DATA ppd = nullptr;
        USHORT reportLen = 0;
        bool isConsumerControl = false;
        if (HidD_GetPreparsedData(h.get(), &ppd)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS &&
                caps.UsagePage == 0x0C && // Standard Consumer Control (ignores COL01 vendor 0xFF00 & COL03 kbd)
                caps.InputReportByteLength > 0) {
                isConsumerControl = true;
                reportLen = caps.InputReportByteLength;
            } else {
                HidD_FreePreparsedData(ppd);
                ppd = nullptr;
            }
        }
        if (!isConsumerControl || !ppd) continue;

        HidEndpoint ep{};
        ep.evt.create(wil::EventOptions::ManualReset);
        if (!ep.evt.is_valid()) {
            HidD_FreePreparsedData(ppd);
            continue;
        }
        ep.handle = std::move(h);
        ep.ppd    = ppd;
        ep.buf.assign(reportLen, 0);
        endpoints.push_back(std::move(ep));
    }
    SetupDiDestroyDeviceInfoList(devInfo);

    m_openCount = static_cast<uint32_t>(endpoints.size());
    if (endpoints.empty()) {
        return;
    }

    auto armRead = [](HidEndpoint& ep) {
        ep.evt.ResetEvent();
        ep.ov = {};
        ep.ov.hEvent = ep.evt.get();
        DWORD readBytes = 0;
        if (!ReadFile(ep.handle.get(), ep.buf.data(), static_cast<DWORD>(ep.buf.size()), &readBytes, &ep.ov)) {
            ep.pending = (GetLastError() == ERROR_IO_PENDING);
        } else {
            ep.pending = true;
            ep.evt.SetEvent();
        }
    };

    std::vector<HANDLE> waitHandles;
    waitHandles.push_back(m_stopEvent.get());
    for (auto& ep : endpoints) {
        armRead(ep);
        waitHandles.push_back(ep.evt.get());
    }

    while (m_running.load(std::memory_order_acquire)) {
        const DWORD res = WaitForMultipleObjects(static_cast<DWORD>(waitHandles.size()),
                                                 waitHandles.data(), FALSE, 500);
        if (res == WAIT_OBJECT_0) {
            break;
        }
        if (res > WAIT_OBJECT_0 && res < WAIT_OBJECT_0 + waitHandles.size()) {
            const size_t epIdx = static_cast<size_t>(res - WAIT_OBJECT_0 - 1);
            HidEndpoint& ep = endpoints[epIdx];
            DWORD got = 0;
            if (GetOverlappedResult(ep.handle.get(), &ep.ov, &got, FALSE) && got > 0 && ep.ppd) {
                USAGE usages[16]{};
                ULONG usageLen = 16;
                if (HidP_GetUsages(HidP_Input, 0x0C, 0, usages, &usageLen, ep.ppd,
                                   reinterpret_cast<PCHAR>(ep.buf.data()), got) == HIDP_STATUS_SUCCESS) {
                    bool muteNow = false;
                    bool volUp   = false;
                    bool volDown = false;
                    for (ULONG u = 0; u < usageLen; ++u) {
                        if (usages[u] == 0xE2) muteNow = true;      // HID Consumer Mute
                        else if (usages[u] == 0xE9) volUp = true;   // HID Volume Increment
                        else if (usages[u] == 0xEA) volDown = true; // HID Volume Decrement
                    }

                    if (muteNow && !ep.prevMuteBtn) {
                        const bool nextMuted = m_sharedState
                            ? !m_sharedState->hardwareMuted.load(std::memory_order_relaxed)
                            : true;
                        if (m_sharedState) {
                            m_sharedState->hardwareMuted.store(nextMuted, std::memory_order_relaxed);
                        }
                        if (m_onMute) {
                            m_onMute(nextMuted);
                        }
                    }
                    if (volUp && !ep.prevVolUp && m_onVolumeStep) {
                        m_onVolumeStep(0.5f);
                    }
                    if (volDown && !ep.prevVolDown && m_onVolumeStep) {
                        m_onVolumeStep(-0.5f);
                    }

                    ep.prevMuteBtn = muteNow;
                    ep.prevVolUp   = volUp;
                    ep.prevVolDown = volDown;
                }
            }
            armRead(ep);
        }
    }

    for (auto& ep : endpoints) {
        if (ep.pending) {
            CancelIoEx(ep.handle.get(), &ep.ov);
            DWORD dummy = 0;
            GetOverlappedResult(ep.handle.get(), &ep.ov, &dummy, TRUE);
            ep.pending = false;
        }
    }
}

} // namespace stoneridge
