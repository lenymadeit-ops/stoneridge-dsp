#include "ipc/SharedState.h"
#include "ipc/ConfigPersistence.h"

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <WebView2.h>
#include <wrl.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>

using json = nlohmann::json;

namespace {

constexpr wchar_t kWindowClass[] = L"StoneridgePanelWindowClass";
constexpr wchar_t kWindowTitle[] = L"Mackie EM-91CU+ - Stoneridge ASIO";
constexpr UINT_PTR kTelemetryTimerId = 1;
constexpr UINT kTimerIntervalMs = 33;
constexpr UINT WM_TRAY = WM_APP + 7;
constexpr UINT ID_TRAY_MUTE = 2001;
constexpr UINT ID_TRAY_TRACK = 2002;
constexpr UINT ID_TRAY_MIX = 2003;
constexpr UINT ID_TRAY_RESTORE = 2004;
constexpr UINT ID_TRAY_QUIT = 2005;
bool g_forceTelemetry = true;
bool g_trayAdded = false;
bool g_quitting = false;
bool g_webviewStarting = false;

HWND g_hwnd = nullptr;
wil::com_ptr<ICoreWebView2Controller> g_webviewController;
wil::com_ptr<ICoreWebView2>           g_webview;
wil::unique_handle                    g_shmHandle;
stoneridge::SharedState*              g_state = nullptr;

bool IsWindowsDarkMode() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return value == 0;
    }
    return true;
}

void UpdateTitleBarTheme(HWND hwnd) {
    if (!g_state) return;
    const auto mode = static_cast<stoneridge::ThemeMode>(g_state->themeMode.load(std::memory_order_relaxed));
    BOOL useDark = TRUE;
    if (mode == stoneridge::ThemeMode::Light) {
        useDark = FALSE;
    } else if (mode == stoneridge::ThemeMode::Auto) {
        useDark = IsWindowsDarkMode() ? TRUE : FALSE;
    }
    constexpr DWORD kDwmUseImmersiveDarkMode = 20;
    DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &useDark, sizeof(useDark));
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
    return s;
}

void PushTelemetryToWebView() {
    if (!g_webview || !g_state) return;
    static float lastPeak = -999.0f;
    static int tick = 0;
    const float peak = g_state->inputPeakDb.load(std::memory_order_relaxed);
    ++tick;
    const bool streaming = g_state->streamRunning.load(std::memory_order_relaxed);
    const bool eco = g_state->ecoMode.load(std::memory_order_relaxed);
    BOOL cloaked = FALSE;
    if (g_hwnd) DwmGetWindowAttribute(g_hwnd, 14, &cloaked, sizeof(cloaked));
    uint32_t learnSt = g_state->learnState.load(std::memory_order_relaxed);
    static uint32_t prevLearn = 0;
    if (learnSt == static_cast<uint32_t>(stoneridge::LearnState::Completed) && prevLearn != learnSt) {
        stoneridge::SaveSharedStateToConfig(g_state);
    }
    prevLearn = learnSt;

    if (cloaked) return;
    if (!g_forceTelemetry && !streaming && std::fabs(peak - lastPeak) < 0.5f) return;
    if (!g_forceTelemetry && eco && (tick % 4) != 0 && std::fabs(peak - lastPeak) < 0.8f) return;
    if (!g_forceTelemetry && (tick % 10) != 0 && std::fabs(peak - lastPeak) < 0.4f) return;
    g_forceTelemetry = false;
    lastPeak = peak;

    json jitterArr = json::array();
    const uint32_t writeIdx = g_state->jitterWriteIndex.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < stoneridge::kJitterHistorySize; ++i) {
        const uint32_t idx = (writeIdx + i) % stoneridge::kJitterHistorySize;
        jitterArr.push_back(g_state->jitterHistoryUs[idx].load(std::memory_order_relaxed));
    }

    json msg = {
        { "type", "state" },
        { "params", {
            { "inputGainDb",          g_state->inputGainDb.load(std::memory_order_relaxed) },
            { "inputMute",            g_state->inputMute.load(std::memory_order_relaxed) },
            { "softClipEnabled",      g_state->softClipEnabled.load(std::memory_order_relaxed) },
            { "audioHpfEnabled",      g_state->audioHpfEnabled.load(std::memory_order_relaxed) },
            { "directMonitorMix",     g_state->directMonitorMix.load(std::memory_order_relaxed) },
            { "hardwarePgaGainDb",      g_state->hardwarePgaGainDb.load(std::memory_order_relaxed) },
            { "hardwareDacGainDb",      g_state->hardwareDacGainDb.load(std::memory_order_relaxed) },
            { "hardwareDacBalanceDb",   g_state->hardwareDacBalanceDb.load(std::memory_order_relaxed) },
            { "autoPadOnClipEnabled",   g_state->autoPadOnClipEnabled.load(std::memory_order_relaxed) },
            { "gateEnabled",          g_state->gateEnabled.load(std::memory_order_relaxed) },
            { "gateLookAheadEnabled", g_state->gateLookAheadEnabled.load(std::memory_order_relaxed) },
            { "gateThresholdDb",      g_state->gateThresholdDb.load(std::memory_order_relaxed) },
            { "gateHysteresisDb",     g_state->gateHysteresisDb.load(std::memory_order_relaxed) },
            { "gateAttackMs",         g_state->gateAttackMs.load(std::memory_order_relaxed) },
            { "gateHoldMs",           g_state->gateHoldMs.load(std::memory_order_relaxed) },
            { "gateReleaseMs",        g_state->gateReleaseMs.load(std::memory_order_relaxed) },
            { "gateRangeDb",          g_state->gateRangeDb.load(std::memory_order_relaxed) },
            { "gateSidechainHpfHz",   g_state->gateSidechainHpfHz.load(std::memory_order_relaxed) },
            { "humNotchFreqHz",       g_state->humNotchFreqHz.load(std::memory_order_relaxed) },
            { "capsuleEqEnabled",     g_state->capsuleEqEnabled.load(std::memory_order_relaxed) },
            { "capsuleCorrectionEnabled",  g_state->capsuleCorrectionEnabled.load(std::memory_order_relaxed) },
            { "capsuleCorrectionStrength", g_state->capsuleCorrectionStrength.load(std::memory_order_relaxed) },
            { "deEsserAmount",        g_state->deEsserAmount.load(std::memory_order_relaxed) },
            { "plosiveGuardEnabled",  g_state->plosiveGuardEnabled.load(std::memory_order_relaxed) },
            { "micModel",             g_state->micModel.load(std::memory_order_relaxed) },
            { "virtualDistanceCm",    g_state->virtualDistanceCm.load(std::memory_order_relaxed) },
            { "preampModel",          g_state->preampModel.load(std::memory_order_relaxed) },
            { "preampDriveDb",        g_state->preampDriveDb.load(std::memory_order_relaxed) },
            { "transformerIronAmount", g_state->transformerIronAmount.load(std::memory_order_relaxed) },
            { "fetPeakCatchAmount",   g_state->fetPeakCatchAmount.load(std::memory_order_relaxed) },
            { "fetRatioMode",         g_state->fetRatioMode.load(std::memory_order_relaxed) },
            { "vocalLevelerAmount",     g_state->vocalLevelerAmount.load(std::memory_order_relaxed) },
            { "consoleEqModel",       g_state->consoleEqModel.load(std::memory_order_relaxed) },
            { "consoleLowDb",         g_state->consoleLowDb.load(std::memory_order_relaxed) },
            { "consoleMidDb",         g_state->consoleMidDb.load(std::memory_order_relaxed) },
            { "consoleAirDb",         g_state->consoleAirDb.load(std::memory_order_relaxed) },
            { "dualTrackSafetyMode",    g_state->dualTrackSafetyMode.load(std::memory_order_relaxed) },
            { "mic2RoutingMode",        g_state->mic2RoutingMode.load(std::memory_order_relaxed) },
            { "abLevelMatchedBypass",   g_state->abLevelMatchedBypass.load(std::memory_order_relaxed) },
            { "monitorReverbAmount",    g_state->monitorReverbAmount.load(std::memory_order_relaxed) },
            { "crossfeedEnabled",       g_state->crossfeedEnabled.load(std::memory_order_relaxed) },
            { "cueCarveEnabled",        g_state->cueCarveEnabled.load(std::memory_order_relaxed) },
            { "ultrasonicLpfEnabled",   g_state->ultrasonicLpfEnabled.load(std::memory_order_relaxed) },
            { "driftAsrcEnabled",       g_state->driftAsrcEnabled.load(std::memory_order_relaxed) },
            { "dacIspGuardEnabled",     g_state->dacIspGuardEnabled.load(std::memory_order_relaxed) },
            { "dacDitherShaperEnabled", g_state->dacDitherShaperEnabled.load(std::memory_order_relaxed) },
            { "engineMode",           g_state->engineMode.load(std::memory_order_relaxed) },
            { "ksIrpCount",           g_state->ksIrpCount.load(std::memory_order_relaxed) },
            { "autopilotEnabled",     g_state->autopilotEnabled.load(std::memory_order_relaxed) },
            { "requestedBufferSize",  g_state->requestedBufferSize.load(std::memory_order_relaxed) },
            { "requestedSampleRate",  g_state->requestedSampleRate.load(std::memory_order_relaxed) },
            { "pCorePriority",        g_state->pCorePriority.load(std::memory_order_relaxed) },
            { "adaptiveXRunGuard",    g_state->adaptiveXRunGuard.load(std::memory_order_relaxed) },
            { "themeMode",            g_state->themeMode.load(std::memory_order_relaxed) },
            { "ecoMode",              g_state->ecoMode.load(std::memory_order_relaxed) },
            { "beautyAmount",         g_state->beautyAmount.load(std::memory_order_relaxed) },
            { "contourAmt",           g_state->contourAmt.load(std::memory_order_relaxed) },
            { "harmonicSculptAmt",    g_state->harmonicSculptAmt.load(std::memory_order_relaxed) },
            { "deHarshAmt",           g_state->deHarshAmt.load(std::memory_order_relaxed) },
            { "parallelGlueAmt",      g_state->parallelGlueAmt.load(std::memory_order_relaxed) },
            { "backingHeadroomDb",    g_state->backingHeadroomDb.load(std::memory_order_relaxed) },
            { "freezeAbxGain",        g_state->freezeAbxGain.load(std::memory_order_relaxed) },
            { "voiceLoopState",       g_state->voiceLoopState.load(std::memory_order_relaxed) },
            { "abAuditionMonitorOnly", g_state->abAuditionMonitorOnly.load(std::memory_order_relaxed) },
            { "wdfEqEnabled",         g_state->wdfEqEnabled.load(std::memory_order_relaxed) },
            { "molecularClip",        g_state->molecularClip.load(std::memory_order_relaxed) }
        }},
        { "telemetry", {
            { "deviceConnected",          g_state->deviceConnected.load(std::memory_order_relaxed) },
            { "captureActive",            g_state->captureActive.load(std::memory_order_relaxed) },
            { "streamRunning",            g_state->streamRunning.load(std::memory_order_relaxed) },
            { "activeEngineMode",         g_state->activeEngineMode.load(std::memory_order_relaxed) },
            { "activeIrpCount",           g_state->activeIrpCount.load(std::memory_order_relaxed) },
            { "activeBufferSize",         g_state->activeBufferSize.load(std::memory_order_relaxed) },
            { "activeSampleRate",         g_state->activeSampleRate.load(std::memory_order_relaxed) },
            { "activeBitDepth",           g_state->activeBitDepth.load(std::memory_order_relaxed) },
            { "inputLatencyMs",           g_state->inputLatencyMs.load(std::memory_order_relaxed) },
            { "outputLatencyMs",          g_state->outputLatencyMs.load(std::memory_order_relaxed) },
            { "roundTripLatencyMs",       g_state->roundTripLatencyMs.load(std::memory_order_relaxed) },
            { "hasHardwarePga",           g_state->hasHardwarePga.load(std::memory_order_relaxed) },
            { "hardwarePgaMinDb",         g_state->hardwarePgaMinDb.load(std::memory_order_relaxed) },
            { "hardwarePgaMaxDb",         g_state->hardwarePgaMaxDb.load(std::memory_order_relaxed) },
            { "hasHardwareDac",           g_state->hasHardwareDac.load(std::memory_order_relaxed) },
            { "hardwareDacMinDb",         g_state->hardwareDacMinDb.load(std::memory_order_relaxed) },
            { "hardwareDacMaxDb",         g_state->hardwareDacMaxDb.load(std::memory_order_relaxed) },
            { "hardwareMuted",            g_state->hardwareMuted.load(std::memory_order_relaxed) },
            { "captureMonoActive",        g_state->captureMonoActive.load(std::memory_order_relaxed) },
            { "supportsWaveRt",           g_state->supportsWaveRt.load(std::memory_order_relaxed) },
            { "inputPeakDb",              g_state->inputPeakDb.load(std::memory_order_relaxed) },
            { "inputRmsDb",               g_state->inputRmsDb.load(std::memory_order_relaxed) },
            { "outputPeakDb",             g_state->outputPeakDb.load(std::memory_order_relaxed) },
            { "outputTruePeakDb",         g_state->outputTruePeakDb.load(std::memory_order_relaxed) },
            { "dacIspReductionDb",        g_state->dacIspReductionDb.load(std::memory_order_relaxed) },
            { "clockDriftPpm",            g_state->clockDriftPpm.load(std::memory_order_relaxed) },
            { "preampCalibActive",        g_state->preampCalibActive.load(std::memory_order_relaxed) },
            { "recommendedPgaDb",         g_state->recommendedPgaDb.load(std::memory_order_relaxed) },
            { "recommendedTrimDb",        g_state->recommendedTrimDb.load(std::memory_order_relaxed) },
            { "gateGainReductionDb",      g_state->gateGainReductionDb.load(std::memory_order_relaxed) },
            { "gateIsOpen",               g_state->gateIsOpen.load(std::memory_order_relaxed) },
            { "deEsserReductionDb",       g_state->deEsserReductionDb.load(std::memory_order_relaxed) },
            { "plosiveReductionDb",       g_state->plosiveReductionDb.load(std::memory_order_relaxed) },
            { "fetReductionDb",           g_state->fetReductionDb.load(std::memory_order_relaxed) },
            { "levelerReductionDb",       g_state->levelerReductionDb.load(std::memory_order_relaxed) },
            { "abMatchGainDb",            g_state->abMatchGainDb.load(std::memory_order_relaxed) },
            { "capsuleCalibrated",        g_state->capsuleCalibrated.load(std::memory_order_relaxed) },
            { "adcClipDetected",          g_state->adcClipDetected.load(std::memory_order_relaxed) },
            { "learnState",               learnSt },
            { "learnProgress",            g_state->learnProgress.load(std::memory_order_relaxed) },
            { "measuredNoiseFloorDb",     g_state->measuredNoiseFloorDb.load(std::memory_order_relaxed) },
            { "callbackCpuPercent",       g_state->callbackCpuPercent.load(std::memory_order_relaxed) },
            { "lastJitterUs",             g_state->lastJitterUs.load(std::memory_order_relaxed) },
            { "maxJitterUs",              g_state->maxJitterUs.load(std::memory_order_relaxed) },
            { "xrunCount",                g_state->xrunCount.load(std::memory_order_relaxed) },
            { "supportedSampleRatesMask", g_state->supportedSampleRatesMask.load(std::memory_order_relaxed) },
            { "deviceName",               std::string(g_state->deviceName) },
            { "windowsDark",              IsWindowsDarkMode() },
            { "jitterHistoryUs",          jitterArr }
        }}
    };

    const auto wideJson = Utf8ToWide(msg.dump());
    g_webview->PostWebMessageAsJson(wideJson.c_str());
}

void HandleWebMessage(const std::wstring& jsonStr) {
    if (!g_state) return;
    try {
        const auto j = json::parse(WideToUtf8(jsonStr));
        const std::string action = j.value("action", "");
        if (action == "setParam") {
            const std::string key = j.value("key", "");
            const auto& val = j["value"];

            if      (key == "inputGainDb")          g_state->inputGainDb.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "inputMute")            g_state->inputMute.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "softClipEnabled")      g_state->softClipEnabled.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "audioHpfEnabled")      g_state->audioHpfEnabled.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "directMonitorMix")     g_state->directMonitorMix.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "hardwarePgaGainDb") {
                g_state->hardwarePgaGainDb.store(val.get<float>(), std::memory_order_relaxed);
                g_state->hardwareCtrlCounter.fetch_add(1, std::memory_order_release);
            } else if (key == "hardwareDacGainDb") {
                g_state->hardwareDacGainDb.store(val.get<float>(), std::memory_order_relaxed);
                g_state->hardwareCtrlCounter.fetch_add(1, std::memory_order_release);
            } else if (key == "hardwareDacBalanceDb") {
                g_state->hardwareDacBalanceDb.store(val.get<float>(), std::memory_order_relaxed);
                g_state->hardwareCtrlCounter.fetch_add(1, std::memory_order_release);
            }
            else if (key == "autoPadOnClipEnabled")   g_state->autoPadOnClipEnabled.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "ultrasonicLpfEnabled")   g_state->ultrasonicLpfEnabled.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "driftAsrcEnabled")       g_state->driftAsrcEnabled.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "dacIspGuardEnabled")     g_state->dacIspGuardEnabled.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "dacDitherShaperEnabled") g_state->dacDitherShaperEnabled.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "gateEnabled")          g_state->gateEnabled.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "gateLookAheadEnabled") g_state->gateLookAheadEnabled.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "gateThresholdDb")      g_state->gateThresholdDb.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "gateHysteresisDb")     g_state->gateHysteresisDb.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "gateAttackMs")         g_state->gateAttackMs.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "gateHoldMs")           g_state->gateHoldMs.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "gateReleaseMs")        g_state->gateReleaseMs.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "gateRangeDb")          g_state->gateRangeDb.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "gateSidechainHpfHz")   g_state->gateSidechainHpfHz.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "humNotchFreqHz")       g_state->humNotchFreqHz.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "capsuleEqEnabled")     g_state->capsuleEqEnabled.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "capsuleCorrectionEnabled")  g_state->capsuleCorrectionEnabled.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "capsuleCorrectionStrength") g_state->capsuleCorrectionStrength.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "deEsserAmount")        g_state->deEsserAmount.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "plosiveGuardEnabled")  g_state->plosiveGuardEnabled.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "micModel")             g_state->micModel.store(val.get<uint32_t>(), std::memory_order_relaxed);
            else if (key == "virtualDistanceCm")    g_state->virtualDistanceCm.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "preampModel")          g_state->preampModel.store(val.get<uint32_t>(), std::memory_order_relaxed);
            else if (key == "preampDriveDb")        g_state->preampDriveDb.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "transformerIronAmount") g_state->transformerIronAmount.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "fetPeakCatchAmount")   g_state->fetPeakCatchAmount.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "fetRatioMode")         g_state->fetRatioMode.store(val.get<uint32_t>(), std::memory_order_relaxed);
            else if (key == "vocalLevelerAmount")   g_state->vocalLevelerAmount.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "consoleEqModel")       g_state->consoleEqModel.store(val.get<uint32_t>(), std::memory_order_relaxed);
            else if (key == "consoleLowDb")         g_state->consoleLowDb.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "consoleMidDb")         g_state->consoleMidDb.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "consoleAirDb")         g_state->consoleAirDb.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "dualTrackSafetyMode")  g_state->dualTrackSafetyMode.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "mic2RoutingMode")      g_state->mic2RoutingMode.store(val.get<uint32_t>(), std::memory_order_relaxed);
            else if (key == "abLevelMatchedBypass") g_state->abLevelMatchedBypass.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "monitorReverbAmount")  g_state->monitorReverbAmount.store(val.get<float>(), std::memory_order_relaxed);
            else if (key == "crossfeedEnabled")     g_state->crossfeedEnabled.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "cueCarveEnabled")      g_state->cueCarveEnabled.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "autopilotEnabled")     g_state->autopilotEnabled.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "pCorePriority")        g_state->pCorePriority.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "adaptiveXRunGuard")    g_state->adaptiveXRunGuard.store(val.get<bool>(), std::memory_order_relaxed);
            else if (key == "themeMode") {
                g_state->themeMode.store(val.get<uint32_t>(), std::memory_order_relaxed);
                UpdateTitleBarTheme(g_hwnd);
            } else if (key == "engineMode") {
                g_state->engineMode.store(val.get<uint32_t>(), std::memory_order_relaxed);
                g_state->configChangeCounter.fetch_add(1, std::memory_order_release);
            } else if (key == "ksIrpCount") {
                g_state->ksIrpCount.store(val.get<uint32_t>(), std::memory_order_relaxed);
                g_state->configChangeCounter.fetch_add(1, std::memory_order_release);
            } else if (key == "requestedBufferSize") {
                g_state->requestedBufferSize.store(val.get<uint32_t>(), std::memory_order_relaxed);
                g_state->configChangeCounter.fetch_add(1, std::memory_order_release);
            } else if (key == "requestedSampleRate") {
                g_state->requestedSampleRate.store(val.get<double>(), std::memory_order_relaxed);
                g_state->configChangeCounter.fetch_add(1, std::memory_order_release);
            } else if (key == "ecoMode") {
                g_state->ecoMode.store(val.get<bool>(), std::memory_order_relaxed);
            } else if (key == "beautyAmount") {
                g_state->beautyAmount.store(val.get<float>(), std::memory_order_relaxed);
            } else if (key == "contourAmt") {
                g_state->contourAmt.store(val.get<float>(), std::memory_order_relaxed);
            } else if (key == "harmonicSculptAmt") {
                g_state->harmonicSculptAmt.store(val.get<float>(), std::memory_order_relaxed);
            } else if (key == "deHarshAmt") {
                g_state->deHarshAmt.store(val.get<float>(), std::memory_order_relaxed);
            } else if (key == "parallelGlueAmt") {
                g_state->parallelGlueAmt.store(val.get<float>(), std::memory_order_relaxed);
            } else if (key == "backingHeadroomDb") {
                g_state->backingHeadroomDb.store(val.get<float>(), std::memory_order_relaxed);
            } else if (key == "voiceLoopState") {
                g_state->voiceLoopState.store(val.get<uint32_t>(), std::memory_order_relaxed);
            } else if (key == "abAuditionMonitorOnly") {
                g_state->abAuditionMonitorOnly.store(val.get<bool>(), std::memory_order_relaxed);
            } else if (key == "freezeAbxGain") {
                g_state->freezeAbxGain.store(val.get<bool>(), std::memory_order_relaxed);
            } else if (key == "wdfEqEnabled") {
                g_state->wdfEqEnabled.store(val.get<bool>(), std::memory_order_relaxed);
            } else if (key == "molecularClip") {
                g_state->molecularClip.store(val.get<bool>(), std::memory_order_relaxed);
            }
            g_forceTelemetry = true;
            stoneridge::SaveSharedStateToConfig(g_state);
        } else if (action == "resetXRuns") {
            g_state->xrunCount.store(0, std::memory_order_relaxed);
            g_state->maxJitterUs.store(0.0f, std::memory_order_relaxed);
        } else if (action == "clearAdcClip") {
            g_state->adcClipDetected.store(false, std::memory_order_relaxed);
        } else if (action == "startLearnNoise") {
            g_state->learnProgress.store(0.0f, std::memory_order_relaxed);
            g_state->preampCalibActive.store(false, std::memory_order_relaxed);
            g_state->learnState.store(static_cast<uint32_t>(stoneridge::LearnState::StartRoom), std::memory_order_release);
        } else if (action == "startPreampAutoLevel") {
            g_state->learnProgress.store(0.0f, std::memory_order_relaxed);
            g_state->preampCalibActive.store(true, std::memory_order_relaxed);
            g_state->learnState.store(static_cast<uint32_t>(stoneridge::LearnState::StartPreampLevel), std::memory_order_release);
        } else if (action == "syncToPlugin") {
            stoneridge::SaveLiveTrackingChain(g_state);
        } else if (action == "applyOptimalPreset") {
            g_state->engineMode.store(static_cast<uint32_t>(stoneridge::EngineMode::WdmKsDirect), std::memory_order_relaxed);
            g_state->ksIrpCount.store(3, std::memory_order_relaxed);
            g_state->requestedSampleRate.store(48000.0, std::memory_order_relaxed);
            g_state->requestedBufferSize.store(96, std::memory_order_relaxed);
            g_state->configChangeCounter.fetch_add(1, std::memory_order_release);
            stoneridge::SaveSharedStateToConfig(g_state);
        }
    } catch (...) {
    }
}

void InitializeWebView2(HWND hwnd) {
    wchar_t exeDir[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    PathRemoveFileSpecW(exeDir);
    const std::wstring webDir = std::wstring(exeDir) + L"\\web";

    wchar_t* localAppData = nullptr;
    std::wstring userDataDir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        userDataDir = std::wstring(localAppData) + L"\\StoneridgeDSP\\WebView2";
        CoTaskMemFree(localAppData);
    }

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataDir.empty() ? nullptr : userDataDir.c_str(), nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd, webDir](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    g_webviewStarting = false;
                    return result;
                }
                env->CreateCoreWebView2Controller(
                    hwnd,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd, webDir](HRESULT res, ICoreWebView2Controller* controller) -> HRESULT {
                            g_webviewStarting = false;
                            if (FAILED(res) || !controller) return res;
                            g_webviewController = controller;
                            g_webviewController->get_CoreWebView2(&g_webview);

                            RECT bounds{};
                            GetClientRect(hwnd, &bounds);
                            g_webviewController->put_Bounds(bounds);

                            wil::com_ptr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(g_webview->get_Settings(&settings))) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                            }

                            wil::com_ptr<ICoreWebView2_3> webview3;
                            if (SUCCEEDED(g_webview->QueryInterface(IID_PPV_ARGS(&webview3)))) {
                                webview3->SetVirtualHostNameToFolderMapping(
                                    L"stoneridge.local", webDir.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            }

                            EventRegistrationToken token{};
                            g_webview->add_WebMessageReceived(
                                Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        wil::unique_cotaskmem_string msg;
                                        if (SUCCEEDED(args->get_WebMessageAsJson(&msg)) && msg) {
                                            HandleWebMessage(msg.get());
                                        }
                                        return S_OK;
                                    }).Get(),
                                &token);

                            g_webview->Navigate(L"https://stoneridge.local/index.html");
                            SetTimer(hwnd, kTelemetryTimerId, kTimerIntervalMs, nullptr);
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

void DestroyWebView() {
    if (g_hwnd) KillTimer(g_hwnd, kTelemetryTimerId);
    if (g_webviewController) {
        g_webviewController->Close();
        g_webviewController.reset();
    }
    g_webview.reset();
}

void UpdateTrayTip(HWND hwnd) {
    if (!g_trayAdded || !g_state) return;
    const bool muted = g_state->inputMute.load(std::memory_order_relaxed);
    const bool clip = g_state->adcClipDetected.load(std::memory_order_relaxed);
    const bool live = g_state->streamRunning.load(std::memory_order_relaxed);
    const wchar_t* tip = L"Stoneridge | Standby";
    if (clip) tip = L"Stoneridge | ADC Clip";
    else if (muted) tip = L"Stoneridge | Muted";
    else if (live) tip = L"Stoneridge | Active";
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    wcscpy_s(nid.szTip, tip);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_MUTE, L"Mute");
    AppendMenuW(menu, MF_STRING, ID_TRAY_TRACK, L"Tracking 96 smp");
    AppendMenuW(menu, MF_STRING, ID_TRAY_MIX, L"Mixing 512 smp");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_RESTORE, L"Restore");
    AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit");
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

void EnterTray(HWND hwnd) {
    KillTimer(hwnd, kTelemetryTimerId);
    DestroyWebView();
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"Stoneridge | Standby");
    if (g_trayAdded) Shell_NotifyIconW(NIM_MODIFY, &nid);
    else g_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    ShowWindow(hwnd, SW_HIDE);
    UpdateTrayTip(hwnd);
}

void LeaveTray(HWND hwnd) {
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    if (!g_webview && !g_webviewStarting) {
        g_webviewStarting = true;
        InitializeWebView2(hwnd);
    }
    const UINT interval = (g_state && g_state->ecoMode.load(std::memory_order_relaxed)) ? 83u : kTimerIntervalMs;
    SetTimer(hwnd, kTelemetryTimerId, interval, nullptr);
    g_forceTelemetry = true;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                EnterTray(hwnd);
                return 0;
            }
            if (g_webviewController) {
                RECT bounds{};
                GetClientRect(hwnd, &bounds);
                g_webviewController->put_Bounds(bounds);
            }
            {
                const UINT interval = (g_state && g_state->ecoMode.load(std::memory_order_relaxed)) ? 83u : kTimerIntervalMs;
                SetTimer(hwnd, kTelemetryTimerId, interval, nullptr);
            }
            return 0;

        case WM_TRAY:
            if (lParam == WM_RBUTTONUP) ShowTrayMenu(hwnd);
            else if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) LeaveTray(hwnd);
            return 0;

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_TRAY_MUTE && g_state) {
                const bool muted = g_state->inputMute.load(std::memory_order_relaxed);
                g_state->inputMute.store(!muted, std::memory_order_relaxed);
                UpdateTrayTip(hwnd);
            } else if (LOWORD(wParam) == ID_TRAY_TRACK && g_state) {
                g_state->requestedBufferSize.store(96u, std::memory_order_relaxed);
                g_state->requestedSampleRate.store(48000.0, std::memory_order_relaxed);
                g_state->configChangeCounter.fetch_add(1, std::memory_order_release);
            } else if (LOWORD(wParam) == ID_TRAY_MIX && g_state) {
                g_state->requestedBufferSize.store(512u, std::memory_order_relaxed);
                g_state->configChangeCounter.fetch_add(1, std::memory_order_release);
            } else if (LOWORD(wParam) == ID_TRAY_RESTORE) {
                LeaveTray(hwnd);
            } else if (LOWORD(wParam) == ID_TRAY_QUIT) {
                g_quitting = true;
                DestroyWindow(hwnd);
            }
            return 0;

        case WM_CLOSE:
            if (!g_quitting) {
                EnterTray(hwnd);
                return 0;
            }
            break;

        case WM_TIMER:
            if (wParam == kTelemetryTimerId) {
                UpdateTrayTip(hwnd);
                PushTelemetryToWebView();
            }
            return 0;

        case WM_SETTINGCHANGE:
            UpdateTitleBarTheme(hwnd);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kTelemetryTimerId);
            if (g_trayAdded) {
                NOTIFYICONDATAW nid{};
                nid.cbSize = sizeof(nid);
                nid.hWnd = hwnd;
                nid.uID = 1;
                Shell_NotifyIconW(NIM_DELETE, &nid);
                g_trayAdded = false;
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        CoUninitialize();
        return 0;
    }

    g_shmHandle.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                         0, sizeof(stoneridge::SharedState),
                                         stoneridge::kSharedMemoryName));
    if (g_shmHandle) {
        const bool alreadyExists = (GetLastError() == ERROR_ALREADY_EXISTS);
        g_state = static_cast<stoneridge::SharedState*>(
            MapViewOfFile(g_shmHandle.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(stoneridge::SharedState)));
        const bool valid = g_state
            && g_state->magic == stoneridge::kSharedStateMagic
            && g_state->version == stoneridge::kSharedStateVersion;
        if (g_state && (!alreadyExists || !valid)) {
            stoneridge::InitializeDefaultSharedState(*g_state);
            stoneridge::LoadConfigToSharedState(g_state);
        }
    }

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = kWindowClass;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    constexpr int kWidth  = 960;
    constexpr int kHeight = 820;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - kWidth) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - kHeight) / 2;

    g_hwnd = CreateWindowExW(
        0, kWindowClass, kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        x, y, kWidth, kHeight,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hwnd) {
        CoUninitialize();
        return 1;
    }

    UpdateTitleBarTheme(g_hwnd);
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    InitializeWebView2(g_hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_state) {
        UnmapViewOfFile(g_state);
        g_state = nullptr;
    }
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
