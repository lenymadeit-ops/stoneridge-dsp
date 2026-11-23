#pragma once

#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <cstdint>
#include <string>
#include <vector>

namespace stoneridge {

struct HardwareCapabilities {
    bool  ksFilterFound       = false;
    bool  usingDirectKsNodes  = false;
    bool  captureSupportsMono = false;
    bool  supportsWaveRt      = false;
    bool  hasSidetoneNode     = false;

    bool  hasInputPgaGain     = false;
    float inputPgaMinDb       = -63.5f;
    float inputPgaMaxDb       = 0.0f;
    float inputPgaStepDb      = 0.5f;
    float inputPgaCurrentDb   = 0.0f;

    bool  hasOutputDacGain    = false;
    float outputDacMinDb      = -63.5f;
    float outputDacMaxDb      = 0.0f;
    float outputDacStepDb     = 0.5f;
    float outputDacCurrentDb  = 0.0f;
    float outputDacBalanceDb  = 0.0f;

    bool  hardwareMuted       = false;
    uint32_t topologyNodeCount = 0;
    uint32_t adcMuteNode      = UINT32_MAX;
    uint32_t adcVolNode       = UINT32_MAX;
    uint32_t dacMuteNode      = UINT32_MAX;
    uint32_t dacVolNode       = UINT32_MAX;
};

// Queries and controls Mackie EM-91CU+ hardware codec capabilities:
// - Direct IOCTL_KS_PROPERTY WDM-KS Topology Nodes (0.5 dB exact ADC PGA, L/R DAC & Mute)
// - Mono Pin 1 support & WaveRT probe
// - Fallback USB Audio Class control via IAudioEndpointVolume
class WdmKsHardwareController {
public:
    WdmKsHardwareController() = default;
    ~WdmKsHardwareController() = default;

    bool Initialize(const std::wstring& ksFilterPath, ULONG capturePinId, ULONG renderPinId);
    void Shutdown();

    const HardwareCapabilities& GetCapabilities() const noexcept { return m_caps; }

    bool SetInputPgaGainDb(float db);
    bool SetOutputDacGainDb(float db);
    bool SetOutputDacGainAndBalanceDb(float masterDb, float balanceDb);
    bool SetHardwareMute(bool muted);
    bool RefreshHardwareState();

    // Queries hardware DMA sample position on a running KS pin
    static bool QueryPinAudioPosition(HANDLE hPin, uint64_t& outPlayPos, uint64_t& outWritePos) noexcept;

private:
    void ProbeKsTopologyAndPins(const std::wstring& ksFilterPath, ULONG capturePinId, ULONG renderPinId);
    void BindEndpointVolumes();
    bool QueryKsNodeVolumeRange(ULONG nodeId, LONG channel, float& outMinDb, float& outMaxDb, float& outStepDb) noexcept;
    bool GetKsNodeVolumeDb(ULONG nodeId, LONG channel, float& outDb) noexcept;
    bool SetKsNodeVolumeDb(ULONG nodeId, LONG channel, float db) noexcept;
    bool GetKsNodeMute(ULONG nodeId, bool& outMuted) noexcept;
    bool SetKsNodeMute(ULONG nodeId, bool muted) noexcept;

    HardwareCapabilities m_caps{};
    wil::unique_handle m_ksFilter;
    wil::com_ptr<IAudioEndpointVolume> m_captureVol;
    wil::com_ptr<IAudioEndpointVolume> m_renderVol;
};

} // namespace stoneridge
