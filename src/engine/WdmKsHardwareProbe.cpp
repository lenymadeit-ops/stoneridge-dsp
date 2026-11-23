#include "engine/WdmKsHardwareProbe.h"
#include "engine/WasapiDeviceManager.h"

#include <winioctl.h>
#include <mmreg.h>
#include <mmsystem.h>
#include <ks.h>
#include <ksmedia.h>
#include <wil/resource.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace stoneridge {

namespace {

bool QueryKsPropSync(HANDLE h, const GUID& set, ULONG id, ULONG flags,
                     const void* extra, DWORD extraSize,
                     void* outBuf, DWORD outSize, DWORD* bytesRet) noexcept {
    if (!h || h == INVALID_HANDLE_VALUE || extraSize > 64) return false;
    BYTE propBuf[sizeof(KSPROPERTY) + 64]{};
    auto* prop  = reinterpret_cast<KSPROPERTY*>(propBuf);
    prop->Set   = set;
    prop->Id    = id;
    prop->Flags = flags;
    if (extra && extraSize > 0) {
        std::memcpy(propBuf + sizeof(KSPROPERTY), extra, extraSize);
    }

    wil::unique_event evt;
    evt.create(wil::EventOptions::ManualReset);
    if (!evt.is_valid()) return false;

    OVERLAPPED ov{};
    ov.hEvent = evt.get();
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_KS_PROPERTY,
                              propBuf, static_cast<DWORD>(sizeof(KSPROPERTY) + extraSize),
                              outBuf, outSize, &ret, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = GetOverlappedResult(h, &ov, &ret, TRUE);
    }
    if (bytesRet) *bytesRet = ret;
    return ok != FALSE;
}

bool QueryKsNodeChannelPropSync(HANDLE h, const GUID& set, ULONG id, ULONG flags,
                                ULONG nodeId, LONG channel,
                                void* outBuf, DWORD outSize, DWORD* bytesRet) noexcept {
    if (!h || h == INVALID_HANDLE_VALUE) return false;
    KSNODEPROPERTY_AUDIO_CHANNEL prop{};
    prop.NodeProperty.Property.Set   = set;
    prop.NodeProperty.Property.Id    = id;
    prop.NodeProperty.Property.Flags = flags | KSPROPERTY_TYPE_TOPOLOGY;
    prop.NodeProperty.NodeId         = nodeId;
    prop.NodeProperty.Reserved       = 0;
    prop.Channel                     = channel;
    prop.Reserved                    = 0;

    wil::unique_event evt;
    evt.create(wil::EventOptions::ManualReset);
    if (!evt.is_valid()) return false;

    OVERLAPPED ov{};
    ov.hEvent = evt.get();
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_KS_PROPERTY,
                              &prop, sizeof(prop),
                              outBuf, outSize, &ret, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = GetOverlappedResult(h, &ov, &ret, TRUE);
    }
    if (bytesRet) *bytesRet = ret;
    return ok != FALSE;
}

float SnapToStep(float db, float minDb, float maxDb, float stepDb) noexcept {
    const float clamped = std::clamp(db, minDb, maxDb);
    if (stepDb <= 0.01f) return clamped;
    return std::clamp(std::round(clamped / stepDb) * stepDb, minDb, maxDb);
}

} // namespace

bool WdmKsHardwareController::Initialize(const std::wstring& ksFilterPath, ULONG capturePinId, ULONG renderPinId) {
    Shutdown();
    ProbeKsTopologyAndPins(ksFilterPath, capturePinId, renderPinId);
    BindEndpointVolumes();
    RefreshHardwareState();
    return m_caps.ksFilterFound || m_caps.hasInputPgaGain || m_caps.hasOutputDacGain;
}

void WdmKsHardwareController::Shutdown() {
    m_captureVol.reset();
    m_renderVol.reset();
    m_ksFilter.reset();
    m_caps = {};
}

bool WdmKsHardwareController::QueryKsNodeVolumeRange(ULONG nodeId, LONG channel,
                                                     float& outMinDb, float& outMaxDb, float& outStepDb) noexcept {
    if (!m_ksFilter || nodeId == UINT32_MAX) return false;
    BYTE buf[256]{};
    DWORD bytes = 0;
    if (!QueryKsNodeChannelPropSync(m_ksFilter.get(), KSPROPSETID_Audio, KSPROPERTY_AUDIO_VOLUMELEVEL,
                                    KSPROPERTY_TYPE_BASICSUPPORT, nodeId, channel, buf, sizeof(buf), &bytes)) {
        return false;
    }
    if (bytes < sizeof(KSPROPERTY_DESCRIPTION) + sizeof(KSPROPERTY_MEMBERSHEADER) + sizeof(KSPROPERTY_STEPPING_LONG)) {
        return false;
    }
    const auto* desc = reinterpret_cast<const KSPROPERTY_DESCRIPTION*>(buf);
    if (desc->MembersListCount == 0) return false;
    const auto* mh = reinterpret_cast<const KSPROPERTY_MEMBERSHEADER*>(buf + sizeof(KSPROPERTY_DESCRIPTION));
    if (mh->MembersFlags != KSPROPERTY_MEMBER_STEPPEDRANGES || mh->MembersCount == 0) {
        return false;
    }
    const auto* sr = reinterpret_cast<const KSPROPERTY_STEPPING_LONG*>(
        buf + sizeof(KSPROPERTY_DESCRIPTION) + sizeof(KSPROPERTY_MEMBERSHEADER));
    outMinDb  = static_cast<float>(sr->Bounds.SignedMinimum) / 65536.0f;
    outMaxDb  = static_cast<float>(sr->Bounds.SignedMaximum) / 65536.0f;
    outStepDb = std::max(0.125f, static_cast<float>(sr->SteppingDelta) / 65536.0f);
    return true;
}

bool WdmKsHardwareController::GetKsNodeVolumeDb(ULONG nodeId, LONG channel, float& outDb) noexcept {
    if (!m_ksFilter || nodeId == UINT32_MAX) return false;
    LONG fixed = 0;
    DWORD bytes = 0;
    if (!QueryKsNodeChannelPropSync(m_ksFilter.get(), KSPROPSETID_Audio, KSPROPERTY_AUDIO_VOLUMELEVEL,
                                    KSPROPERTY_TYPE_GET, nodeId, channel, &fixed, sizeof(fixed), &bytes)) {
        return false;
    }
    outDb = static_cast<float>(fixed) / 65536.0f;
    return true;
}

bool WdmKsHardwareController::SetKsNodeVolumeDb(ULONG nodeId, LONG channel, float db) noexcept {
    if (!m_ksFilter || nodeId == UINT32_MAX) return false;
    LONG fixed = static_cast<LONG>(std::lround(db * 65536.0f));
    DWORD bytes = 0;
    return QueryKsNodeChannelPropSync(m_ksFilter.get(), KSPROPSETID_Audio, KSPROPERTY_AUDIO_VOLUMELEVEL,
                                      KSPROPERTY_TYPE_SET, nodeId, channel, &fixed, sizeof(fixed), &bytes);
}

bool WdmKsHardwareController::GetKsNodeMute(ULONG nodeId, bool& outMuted) noexcept {
    if (!m_ksFilter || nodeId == UINT32_MAX) return false;
    BOOL m = FALSE;
    DWORD bytes = 0;
    if (!QueryKsNodeChannelPropSync(m_ksFilter.get(), KSPROPSETID_Audio, KSPROPERTY_AUDIO_MUTE,
                                    KSPROPERTY_TYPE_GET, nodeId, 0, &m, sizeof(m), &bytes)) {
        return false;
    }
    outMuted = (m != FALSE);
    return true;
}

bool WdmKsHardwareController::SetKsNodeMute(ULONG nodeId, bool muted) noexcept {
    if (!m_ksFilter || nodeId == UINT32_MAX) return false;
    BOOL m = muted ? TRUE : FALSE;
    DWORD bytes = 0;
    return QueryKsNodeChannelPropSync(m_ksFilter.get(), KSPROPSETID_Audio, KSPROPERTY_AUDIO_MUTE,
                                      KSPROPERTY_TYPE_SET, nodeId, 0, &m, sizeof(m), &bytes);
}

void WdmKsHardwareController::ProbeKsTopologyAndPins(const std::wstring& ksFilterPath,
                                                     ULONG capturePinId,
                                                     ULONG renderPinId) {
    if (ksFilterPath.empty()) return;

    m_ksFilter.reset(CreateFileW(ksFilterPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (!m_ksFilter) return;

    m_caps.ksFilterFound = true;

    // 1. Enumerate Topology Nodes
    std::vector<GUID> nodeTypes;
    DWORD bytes = 0;
    QueryKsPropSync(m_ksFilter.get(), KSPROPSETID_Topology, KSPROPERTY_TOPOLOGY_NODES,
                    KSPROPERTY_TYPE_GET, nullptr, 0, nullptr, 0, &bytes);
    if (bytes >= sizeof(KSMULTIPLE_ITEM)) {
        std::vector<BYTE> nodeBuf(bytes);
        if (QueryKsPropSync(m_ksFilter.get(), KSPROPSETID_Topology, KSPROPERTY_TOPOLOGY_NODES,
                            KSPROPERTY_TYPE_GET, nullptr, 0, nodeBuf.data(), bytes, &bytes)) {
            const auto* multi = reinterpret_cast<const KSMULTIPLE_ITEM*>(nodeBuf.data());
            const auto* guids = reinterpret_cast<const GUID*>(nodeBuf.data() + sizeof(KSMULTIPLE_ITEM));
            m_caps.topologyNodeCount = multi->Count;
            nodeTypes.assign(guids, guids + multi->Count);
            for (const auto& g : nodeTypes) {
                if (g == KSNODETYPE_SUM || g == KSNODETYPE_MUX) {
                    m_caps.hasSidetoneNode = true;
                }
            }
        }
    }

    // 2. Walk Topology Connections to bind exact Render & Capture MUTE / VOLUME node IDs
    bytes = 0;
    QueryKsPropSync(m_ksFilter.get(), KSPROPSETID_Topology, KSPROPERTY_TOPOLOGY_CONNECTIONS,
                    KSPROPERTY_TYPE_GET, nullptr, 0, nullptr, 0, &bytes);
    if (!nodeTypes.empty() && bytes >= sizeof(KSMULTIPLE_ITEM)) {
        std::vector<BYTE> connBuf(bytes);
        if (QueryKsPropSync(m_ksFilter.get(), KSPROPSETID_Topology, KSPROPERTY_TOPOLOGY_CONNECTIONS,
                            KSPROPERTY_TYPE_GET, nullptr, 0, connBuf.data(), bytes, &bytes)) {
            const auto* multi = reinterpret_cast<const KSMULTIPLE_ITEM*>(connBuf.data());
            const auto* conns = reinterpret_cast<const KSTOPOLOGY_CONNECTION*>(connBuf.data() + sizeof(KSMULTIPLE_ITEM));

            // Walk forward from Render Pin (e.g. Pin 0 -> Node 0 -> Node 1 Mute -> Node 2 Vol)
            if (renderPinId != UINT32_MAX) {
                ULONG curFromNode = KSFILTER_NODE;
                ULONG curFromPin  = renderPinId;
                for (ULONG hop = 0; hop < nodeTypes.size() + 2; ++hop) {
                    bool moved = false;
                    for (ULONG c = 0; c < multi->Count; ++c) {
                        if (conns[c].FromNode == curFromNode &&
                            (curFromNode != KSFILTER_NODE || conns[c].FromNodePin == curFromPin)) {
                            const ULONG toNode = conns[c].ToNode;
                            if (toNode == KSFILTER_NODE || toNode >= nodeTypes.size()) {
                                hop = static_cast<ULONG>(nodeTypes.size() + 2);
                                break;
                            }
                            if (nodeTypes[toNode] == KSNODETYPE_MUTE && m_caps.dacMuteNode == UINT32_MAX) {
                                m_caps.dacMuteNode = toNode;
                            } else if (nodeTypes[toNode] == KSNODETYPE_VOLUME && m_caps.dacVolNode == UINT32_MAX) {
                                m_caps.dacVolNode = toNode;
                            }
                            curFromNode = toNode;
                            moved = true;
                            break;
                        }
                    }
                    if (!moved) break;
                }
            }

            // Walk backward from Capture Pin (e.g. Pin 1 <- Node 7 <- Node 6 Vol <- Node 5 Mute)
            if (capturePinId != UINT32_MAX) {
                ULONG curToNode = KSFILTER_NODE;
                ULONG curToPin  = capturePinId;
                for (ULONG hop = 0; hop < nodeTypes.size() + 2; ++hop) {
                    bool moved = false;
                    for (ULONG c = 0; c < multi->Count; ++c) {
                        if (conns[c].ToNode == curToNode &&
                            (curToNode != KSFILTER_NODE || conns[c].ToNodePin == curToPin)) {
                            const ULONG fromNode = conns[c].FromNode;
                            if (fromNode == KSFILTER_NODE || fromNode >= nodeTypes.size()) {
                                hop = static_cast<ULONG>(nodeTypes.size() + 2);
                                break;
                            }
                            if (nodeTypes[fromNode] == KSNODETYPE_VOLUME && m_caps.adcVolNode == UINT32_MAX) {
                                m_caps.adcVolNode = fromNode;
                            } else if (nodeTypes[fromNode] == KSNODETYPE_MUTE && m_caps.adcMuteNode == UINT32_MAX) {
                                m_caps.adcMuteNode = fromNode;
                            }
                            curToNode = fromNode;
                            moved = true;
                            break;
                        }
                    }
                    if (!moved) break;
                }
            }
        }
    }

    // Query direct KS node volume ranges (exact 0.5 dB silicon grid)
    if (m_caps.adcVolNode != UINT32_MAX &&
        QueryKsNodeVolumeRange(m_caps.adcVolNode, 0,
                               m_caps.inputPgaMinDb, m_caps.inputPgaMaxDb, m_caps.inputPgaStepDb)) {
        m_caps.hasInputPgaGain   = true;
        m_caps.usingDirectKsNodes = true;
    }
    if (m_caps.dacVolNode != UINT32_MAX &&
        QueryKsNodeVolumeRange(m_caps.dacVolNode, 0,
                               m_caps.outputDacMinDb, m_caps.outputDacMaxDb, m_caps.outputDacStepDb)) {
        m_caps.hasOutputDacGain  = true;
        m_caps.usingDirectKsNodes = true;
    }

    // 3. Check Capture Pin 1 native 1ch mono datarange
    if (capturePinId != UINT32_MAX) {
        ULONG extra[2] = { capturePinId, 0 };
        DWORD rangeBytes = 0;
        QueryKsPropSync(m_ksFilter.get(), KSPROPSETID_Pin, KSPROPERTY_PIN_DATARANGES,
                        KSPROPERTY_TYPE_GET, extra, sizeof(extra), nullptr, 0, &rangeBytes);
        if (rangeBytes > sizeof(KSMULTIPLE_ITEM)) {
            std::vector<BYTE> rbuf(rangeBytes);
            if (QueryKsPropSync(m_ksFilter.get(), KSPROPSETID_Pin, KSPROPERTY_PIN_DATARANGES,
                                KSPROPERTY_TYPE_GET, extra, sizeof(extra), rbuf.data(), rangeBytes, &rangeBytes)) {
                const auto* multi = reinterpret_cast<const KSMULTIPLE_ITEM*>(rbuf.data());
                const BYTE* ptr = rbuf.data() + sizeof(KSMULTIPLE_ITEM);
                for (ULONG r = 0; r < multi->Count; ++r) {
                    const auto* dr = reinterpret_cast<const KSDATARANGE*>(ptr);
                    if (dr->MajorFormat == KSDATAFORMAT_TYPE_AUDIO &&
                        dr->SubFormat == KSDATAFORMAT_SUBTYPE_PCM &&
                        dr->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
                        const auto* aud = reinterpret_cast<const KSDATARANGE_AUDIO*>(dr);
                        if (aud->MaximumChannels == 1) {
                            m_caps.captureSupportsMono = true;
                        }
                    }
                    ptr += (dr->FormatSize + 7) & ~7;
                }
            }
        }
    }

    // 4. Probe WaveRT (KSPROPSETID_RtAudio) availability on Render pin
    if (renderPinId != UINT32_MAX) {
        ULONG extra[2] = { renderPinId, 0 };
        ULONG propSupport = 0;
        if (QueryKsPropSync(m_ksFilter.get(), KSPROPSETID_RtAudio, KSPROPERTY_RTAUDIO_BUFFER,
                            KSPROPERTY_TYPE_BASICSUPPORT, extra, sizeof(extra),
                            &propSupport, sizeof(propSupport), &bytes)) {
            m_caps.supportsWaveRt = (propSupport != 0);
        }
    }
}

void WdmKsHardwareController::BindEndpointVolumes() {
    WasapiDeviceManager mgr;
    wil::com_ptr<IMMDevice> cap, ren;
    std::wstring friendly;
    bool isMackie = false;
    if (!mgr.FindMackieEndpoints(cap, ren, friendly, isMackie) || !isMackie) {
        return;
    }

    if (cap) {
        if (SUCCEEDED(cap->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(m_captureVol.put())))) {
            if (!m_caps.hasInputPgaGain) {
                float minDb = 0.0f, maxDb = 0.0f, stepDb = 0.5f;
                if (SUCCEEDED(m_captureVol->GetVolumeRange(&minDb, &maxDb, &stepDb))) {
                    m_caps.hasInputPgaGain = true;
                    m_caps.inputPgaMinDb   = minDb;
                    m_caps.inputPgaMaxDb   = maxDb;
                    m_caps.inputPgaStepDb  = stepDb;
                }
            }
        }
    }

    if (ren) {
        if (SUCCEEDED(ren->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(m_renderVol.put())))) {
            if (!m_caps.hasOutputDacGain) {
                float minDb = 0.0f, maxDb = 0.0f, stepDb = 0.5f;
                if (SUCCEEDED(m_renderVol->GetVolumeRange(&minDb, &maxDb, &stepDb))) {
                    m_caps.hasOutputDacGain = true;
                    m_caps.outputDacMinDb   = minDb;
                    m_caps.outputDacMaxDb   = maxDb;
                    m_caps.outputDacStepDb  = stepDb;
                }
            }
        }
    }
}

bool WdmKsHardwareController::RefreshHardwareState() {
    bool ok = false;
    float db = 0.0f;
    if (GetKsNodeVolumeDb(m_caps.adcVolNode, 0, db)) {
        m_caps.inputPgaCurrentDb = SnapToStep(db, m_caps.inputPgaMinDb, m_caps.inputPgaMaxDb, m_caps.inputPgaStepDb);
        ok = true;
    } else if (m_captureVol && SUCCEEDED(m_captureVol->GetMasterVolumeLevel(&db))) {
        m_caps.inputPgaCurrentDb = db;
        ok = true;
    }

    bool muted = false;
    if (GetKsNodeMute(m_caps.adcMuteNode, muted)) {
        m_caps.hardwareMuted = muted;
    } else if (m_captureVol) {
        BOOL wMuted = FALSE;
        if (SUCCEEDED(m_captureVol->GetMute(&wMuted))) {
            m_caps.hardwareMuted = (wMuted != FALSE);
        }
    }

    float ch0 = 0.0f, ch1 = 0.0f;
    if (GetKsNodeVolumeDb(m_caps.dacVolNode, 0, ch0)) {
        if (GetKsNodeVolumeDb(m_caps.dacVolNode, 1, ch1)) {
            m_caps.outputDacCurrentDb = SnapToStep(std::max(ch0, ch1), m_caps.outputDacMinDb, m_caps.outputDacMaxDb, m_caps.outputDacStepDb);
            m_caps.outputDacBalanceDb = SnapToStep(ch1 - ch0, -6.0f, 6.0f, 0.5f);
        } else {
            m_caps.outputDacCurrentDb = SnapToStep(ch0, m_caps.outputDacMinDb, m_caps.outputDacMaxDb, m_caps.outputDacStepDb);
        }
        ok = true;
    } else if (m_renderVol && SUCCEEDED(m_renderVol->GetMasterVolumeLevel(&db))) {
        m_caps.outputDacCurrentDb = db;
        ok = true;
    }
    return ok;
}

bool WdmKsHardwareController::SetInputPgaGainDb(float db) {
    const float snapped = SnapToStep(db, m_caps.inputPgaMinDb, m_caps.inputPgaMaxDb, m_caps.inputPgaStepDb);
    if (SetKsNodeVolumeDb(m_caps.adcVolNode, 0, snapped)) {
        m_caps.inputPgaCurrentDb = snapped;
        return true;
    }
    if (!m_captureVol) return false;
    if (SUCCEEDED(m_captureVol->SetMasterVolumeLevel(snapped, nullptr))) {
        m_caps.inputPgaCurrentDb = snapped;
        return true;
    }
    return false;
}

bool WdmKsHardwareController::SetOutputDacGainDb(float db) {
    return SetOutputDacGainAndBalanceDb(db, m_caps.outputDacBalanceDb);
}

bool WdmKsHardwareController::SetOutputDacGainAndBalanceDb(float masterDb, float balanceDb) {
    const float clampedMaster = SnapToStep(masterDb, m_caps.outputDacMinDb, m_caps.outputDacMaxDb, m_caps.outputDacStepDb);
    const float clampedBal    = SnapToStep(balanceDb, -6.0f, 6.0f, 0.5f);

    // Passive cut on the opposite ear so neither channel ever exceeds 0 dBFS hardware ceiling
    const float leftDb  = SnapToStep((clampedBal > 0.0f) ? (clampedMaster - clampedBal) : clampedMaster,
                                     m_caps.outputDacMinDb, m_caps.outputDacMaxDb, m_caps.outputDacStepDb);
    const float rightDb = SnapToStep((clampedBal < 0.0f) ? (clampedMaster + clampedBal) : clampedMaster,
                                     m_caps.outputDacMinDb, m_caps.outputDacMaxDb, m_caps.outputDacStepDb);

    if (m_caps.dacVolNode != UINT32_MAX && m_ksFilter) {
        const bool okL = SetKsNodeVolumeDb(m_caps.dacVolNode, 0, leftDb);
        const bool okR = SetKsNodeVolumeDb(m_caps.dacVolNode, 1, rightDb);
        if (okL || okR) {
            m_caps.outputDacCurrentDb = clampedMaster;
            m_caps.outputDacBalanceDb = clampedBal;
            return true;
        }
    }

    if (!m_renderVol) return false;
    if (SUCCEEDED(m_renderVol->SetMasterVolumeLevel(clampedMaster, nullptr))) {
        m_caps.outputDacCurrentDb = clampedMaster;
        m_caps.outputDacBalanceDb = clampedBal;
        return true;
    }
    return false;
}

bool WdmKsHardwareController::SetHardwareMute(bool muted) {
    if (SetKsNodeMute(m_caps.adcMuteNode, muted)) {
        m_caps.hardwareMuted = muted;
        return true;
    }
    if (!m_captureVol) return false;
    if (SUCCEEDED(m_captureVol->SetMute(muted ? TRUE : FALSE, nullptr))) {
        m_caps.hardwareMuted = muted;
        return true;
    }
    return false;
}

bool WdmKsHardwareController::QueryPinAudioPosition(HANDLE hPin, uint64_t& outPlayPos, uint64_t& outWritePos) noexcept {
    KSAUDIO_POSITION pos{};
    DWORD bytes = 0;
    if (QueryKsPropSync(hPin, KSPROPSETID_Audio, KSPROPERTY_AUDIO_POSITION,
                        KSPROPERTY_TYPE_GET, nullptr, 0, &pos, sizeof(pos), &bytes)) {
        outPlayPos  = pos.PlayOffset;
        outWritePos = pos.WriteOffset;
        return true;
    }
    return false;
}

} // namespace stoneridge
