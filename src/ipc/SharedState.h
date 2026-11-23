#pragma once

#include <atomic>
#include <cstdint>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace stoneridge {

inline constexpr wchar_t kSharedMemoryName[] = L"Local\\StoneridgeASIO_SharedState_v7";
inline constexpr uint32_t kSharedStateMagic   = 0x53524453; // 'SRDS'
inline constexpr uint32_t kSharedStateVersion = 7;
inline constexpr uint32_t kJitterHistorySize  = 64;

enum class ThemeMode : uint32_t {
    Auto  = 0,
    Dark  = 1,
    Light = 2
};

enum class EngineMode : uint32_t {
    WdmKsDirect     = 0,
    WasapiExclusive = 1
};

enum class LearnState : uint32_t {
    Idle             = 0,
    StartRoom        = 1, // Request 2-sec room-noise & hum calibration
    Sampling         = 2,
    Completed        = 3,
    StartPreampLevel = 4  // Request 3-sec preamp sweet-spot calibration (-9 dBFS peak)
};

struct alignas(64) SharedState {
    uint32_t magic;
    uint32_t version;

    // --- UI -> Audio Engine Parameters ---
    alignas(64) std::atomic<float> inputGainDb;        // -24.0 to +24.0 dB
    std::atomic<bool>  inputMute;                      // Mute toggle
    std::atomic<bool>  softClipEnabled;                // Polynomial ADAA1 safety limiter (-0.3 dBFS)
    std::atomic<bool>  audioHpfEnabled;                // 80 Hz rumble filter
    std::atomic<float> directMonitorMix;               // 0.0 to 1.0 post-gate zero-latency headphone blend

    // Hardware Codec Controls (Mackie EM-91CU+ USB Audio Feature Unit / WDM-KS Nodes / HID)
    std::atomic<float>    hardwarePgaGainDb;           // Hardware ADC PGA Gain (dB, Node[6])
    std::atomic<float>    hardwareDacGainDb;           // Hardware Headphone DAC Volume (dB, Node[2])
    std::atomic<float>    hardwareDacBalanceDb;        // Hardware Headphone L/R Balance (-6..+6 dB, Node[2] ch0/ch1)
    std::atomic<bool>     autoPadOnClipEnabled;        // Opt-in HW PGA -2 dB auto-pad on sustained ADC clip
    std::atomic<uint32_t> hardwareCtrlCounter;         // Bumped by UI when hardware gain/balance changes

    // Noise Gate
    std::atomic<bool>  gateEnabled;
    std::atomic<bool>  gateLookAheadEnabled;           // 1.0 ms transient look-ahead (default false)
    std::atomic<float> gateThresholdDb;                // -80.0 to 0.0 dBFS
    std::atomic<float> gateHysteresisDb;               // 0.0 to 12.0 dB
    std::atomic<float> gateAttackMs;                   // 0.1 to 50.0 ms
    std::atomic<float> gateHoldMs;                     // 5.0 to 500.0 ms
    std::atomic<float> gateReleaseMs;                  // 10.0 to 1000.0 ms
    std::atomic<float> gateRangeDb;                    // -80.0 to 0.0 dB
    std::atomic<float> gateSidechainHpfHz;             // 20.0 to 250.0 Hz (0 = bypass)

    // Studio Microphone Capture DSP Upgrades
    std::atomic<float> humNotchFreqHz;                 // 0 (Off), 50, or 60 Hz (+ 2nd harmonic)
    std::atomic<bool>  capsuleEqEnabled;               // EM-91CU+ minimum-phase voice contour
    std::atomic<bool>  capsuleCorrectionEnabled;       // Measured Pixel 11 Pro MEMS capsule correction
    std::atomic<float> capsuleCorrectionStrength;      // 0.0 to 1.0 (default 0.85)
    std::atomic<float> deEsserAmount;                  // 0.0 to 1.0 (5.5-9 kHz dynamic sibilance control)
    std::atomic<bool>  plosiveGuardEnabled;            // Dynamic <140 Hz pop/plosive suppressor
    std::atomic<float> fetPeakCatchAmount;             // 0.0 to 1.0 (1176-style 80 us feedback FET peak catcher)
    std::atomic<uint32_t> fetRatioMode;                // 0 = 4:1, 1 = 8:1, 2 = British 20:1
    std::atomic<float> vocalLevelerAmount;             // 0.0 to 1.0 (LA-2A T4B dual-memory opto leveler)
    std::atomic<uint32_t> micModel;                    // 0=Bypass, 1=C800G, 2=U87Ai, 3=251E, 4=U47, 5=SM7B
    std::atomic<float> virtualDistanceCm;              // 5.0 to 35.0 cm spherical-wave proximity (15=neutral)
    std::atomic<uint32_t> preampModel;                 // 0=Bypass, 1=Neve1073, 2=API312, 3=V76, 4=SSL9000J
    std::atomic<float> preampDriveDb;                  // 0.0 to 18.0 dB internal saturation drive
    std::atomic<float> transformerIronAmount;          // 0.0 to 1.0 Jiles-Atherton hysteresis mix
    std::atomic<uint32_t> consoleEqModel;              // 0=Bypass, 1=Pultec, 2=1073, 3=SSL4000G, 4=API550A
    std::atomic<float> consoleLowDb;                   // -6.0 to +6.0 dB
    std::atomic<float> consoleMidDb;                   // -6.0 to +6.0 dB
    std::atomic<float> consoleAirDb;                   // -6.0 to +8.0 dB
    std::atomic<bool>  dualTrackSafetyMode;            // Legacy mirror of mic2RoutingMode == 2
    std::atomic<uint32_t> mic2RoutingMode;             // 0=Mirror, 1=Pre-Dynamics Stem, 2=Raw Safety
    std::atomic<bool>  abLevelMatchedBypass;           // K-weighted ITU-R BS.1770 level-matched raw A/B
    std::atomic<bool>  abAuditionMonitorOnly;          // true: raw A/B is headphones only, the take stays processed
    std::atomic<bool>  ecoMode;
    std::atomic<bool>  freezeAbxGain;
    std::atomic<bool>  wdfEqEnabled;
    std::atomic<bool>  molecularClip;
    std::atomic<float> beautyAmount;
    std::atomic<float> contourAmt;
    std::atomic<float> harmonicSculptAmt;
    std::atomic<float> deHarshAmt;
    std::atomic<float> parallelGlueAmt;
    std::atomic<float> backingHeadroomDb;
    std::atomic<uint32_t> voiceLoopState;              // 0 idle, 1 record, 2 loop
    std::atomic<bool>  ultrasonicLpfEnabled;           // Auto 22 kHz LPF at >=88.2 kHz (strips 96k/16b sigma-delta noise)

    // Control-Room Headphone Matrix (Monitor-Only, never printed to DAW)
    std::atomic<float> monitorReverbAmount;            // 0.0 to 1.0 (4x4 Paraunitary FDN Comfort Chamber)
    std::atomic<bool>  crossfeedEnabled;               // BS2B-style 300 us speaker crossfeed
    std::atomic<bool>  cueCarveEnabled;                // -1.8 dB @ 2.2 kHz anti-bleed backing duck while singing

    // DAC & Clocking Conditioning
    std::atomic<bool>  driftAsrcEnabled;               // PI-locked Async ADC <-> Adaptive DAC fractional resampler
    std::atomic<bool>  dacIspGuardEnabled;             // ITU-R BS.1770-4 4x True-Peak ISP Limiter (-0.5 dBTP)
    std::atomic<bool>  dacDitherShaperEnabled;         // 24-bit TPDF Dither + 96kHz 16-bit 2nd-order Noise Shaper

    // 1-Click Room Noise Learning
    std::atomic<uint32_t> learnState;                  // LearnState

    // --- Engine Configuration Requests ---
    std::atomic<uint32_t> engineMode;                  // EngineMode
    std::atomic<uint32_t> ksIrpCount;                  // 2, 3, or 4
    std::atomic<bool>     autopilotEnabled;            // Dynamic Zero-Crackle IRP Autopilot (ON by default)
    std::atomic<uint32_t> requestedBufferSize;         // 64..1024
    std::atomic<double>   requestedSampleRate;         // 44100, 48000, 96000
    std::atomic<bool>     pCorePriority;               // EcoQoS bypass + Core-0 DPC avoidance
    std::atomic<bool>     adaptiveXRunGuard;           // Overload guard
    std::atomic<uint32_t> configChangeCounter;

    // --- UI Preferences ---
    std::atomic<uint32_t> themeMode;                   // ThemeMode (Auto/Dark/Light)

    // --- Audio Engine -> UI Telemetry ---
    alignas(64) std::atomic<bool>     deviceConnected;
    std::atomic<bool>     captureActive;
    std::atomic<bool>     streamRunning;
    std::atomic<uint32_t> activeEngineMode;
    std::atomic<uint32_t> activeIrpCount;              // Live IRP count (2..4, updated by Autopilot)
    std::atomic<uint32_t> activeBufferSize;
    std::atomic<double>   activeSampleRate;
    std::atomic<uint32_t> activeBitDepth;
    std::atomic<uint32_t> inputChannels;
    std::atomic<uint32_t> outputChannels;
    std::atomic<float>    inputLatencyMs;
    std::atomic<float>    outputLatencyMs;
    std::atomic<float>    roundTripLatencyMs;

    // Hardware Probe Capabilities
    std::atomic<bool>     hasHardwarePga;
    std::atomic<float>    hardwarePgaMinDb;
    std::atomic<float>    hardwarePgaMaxDb;
    std::atomic<bool>     hasHardwareDac;
    std::atomic<float>    hardwareDacMinDb;
    std::atomic<float>    hardwareDacMaxDb;
    std::atomic<bool>     hardwareMuted;               // Synced from HID MI_03 / EndpointVolume
    std::atomic<bool>     captureMonoActive;           // True if Pin 1 runs native 1ch mono capture
    std::atomic<bool>     supportsWaveRt;

    // Live DSP & Capture Meters
    std::atomic<float>    inputPeakDb;
    std::atomic<float>    inputRmsDb;
    std::atomic<float>    outputPeakDb;
    std::atomic<float>    outputTruePeakDb;            // ITU-R BS.1770-4 4x oversampled True Peak (dBTP)
    std::atomic<float>    dacIspReductionDb;           // Active ISP limiter attenuation (dB)
    std::atomic<float>    gateGainReductionDb;
    std::atomic<bool>     gateIsOpen;
    std::atomic<float>    deEsserReductionDb;
    std::atomic<float>    plosiveReductionDb;
    std::atomic<float>    fetReductionDb;              // 1176 FET peak catcher GR (dB)
    std::atomic<float>    levelerReductionDb;          // LA-2A T4B opto leveler GR (dB)
    std::atomic<bool>     adcClipDetected;             // Pre-DSP raw hardware ADC clip warning
    std::atomic<float>    learnProgress;               // 0.0 to 1.0
    std::atomic<float>    measuredNoiseFloorDb;
    std::atomic<bool>     preampCalibActive;           // True while 3s Preamp Auto-Level is sampling
    std::atomic<float>    recommendedPgaDb;            // Suggested optimal HW PGA after 3s Auto-Level
    std::atomic<float>    recommendedTrimDb;           // Suggested Digital Trim makeup after 3s Auto-Level
    std::atomic<float>    abMatchGainDb;               // K-weighted level-match compensation (dB)
    std::atomic<float>    dspCpuPercent;               // Input + Monitor DSP CPU share (%)
    std::atomic<bool>     capsuleCalibrated;           // True once Pixel 11 Pro MEMS curve is compiled in
    std::atomic<float>    clockDriftPpm;               // Live ASRC fractional lock offset in ppm

    std::atomic<float>    callbackCpuPercent;
    std::atomic<float>    lastJitterUs;
    std::atomic<float>    maxJitterUs;
    std::atomic<uint32_t> xrunCount;
    std::atomic<uint32_t> supportedSampleRatesMask;

    std::atomic<float>    jitterHistoryUs[kJitterHistorySize];
    std::atomic<uint32_t> jitterWriteIndex;

    char deviceName[128];
};

inline void InitializeDefaultSharedState(SharedState& state) {
    state.magic = kSharedStateMagic;
    state.version = kSharedStateVersion;

    state.inputGainDb.store(0.0f, std::memory_order_relaxed);
    state.inputMute.store(false, std::memory_order_relaxed);
    state.softClipEnabled.store(true, std::memory_order_relaxed);
    state.audioHpfEnabled.store(false, std::memory_order_relaxed);
    state.directMonitorMix.store(0.0f, std::memory_order_relaxed);

    state.hardwarePgaGainDb.store(0.0f, std::memory_order_relaxed);
    state.hardwareDacGainDb.store(0.0f, std::memory_order_relaxed);
    state.hardwareDacBalanceDb.store(0.0f, std::memory_order_relaxed);
    state.autoPadOnClipEnabled.store(false, std::memory_order_relaxed);
    state.hardwareCtrlCounter.store(0, std::memory_order_relaxed);

    state.gateEnabled.store(false, std::memory_order_relaxed);
    state.gateLookAheadEnabled.store(false, std::memory_order_relaxed);
    state.gateThresholdDb.store(-42.0f, std::memory_order_relaxed);
    state.gateHysteresisDb.store(4.0f, std::memory_order_relaxed);
    state.gateAttackMs.store(1.5f, std::memory_order_relaxed);
    state.gateHoldMs.store(60.0f, std::memory_order_relaxed);
    state.gateReleaseMs.store(140.0f, std::memory_order_relaxed);
    state.gateRangeDb.store(-60.0f, std::memory_order_relaxed);
    state.gateSidechainHpfHz.store(90.0f, std::memory_order_relaxed);

    state.humNotchFreqHz.store(0.0f, std::memory_order_relaxed);
    state.capsuleEqEnabled.store(false, std::memory_order_relaxed);
    state.capsuleCorrectionEnabled.store(false, std::memory_order_relaxed);
    state.capsuleCorrectionStrength.store(0.85f, std::memory_order_relaxed);
    state.deEsserAmount.store(0.0f, std::memory_order_relaxed);
    state.plosiveGuardEnabled.store(false, std::memory_order_relaxed);
    state.micModel.store(0u, std::memory_order_relaxed);
    state.virtualDistanceCm.store(15.0f, std::memory_order_relaxed);
    state.fetPeakCatchAmount.store(0.0f, std::memory_order_relaxed);
    state.fetRatioMode.store(0u, std::memory_order_relaxed);
    state.vocalLevelerAmount.store(0.0f, std::memory_order_relaxed);
    state.preampModel.store(0u, std::memory_order_relaxed);
    state.preampDriveDb.store(0.0f, std::memory_order_relaxed);
    state.transformerIronAmount.store(0.50f, std::memory_order_relaxed);
    state.consoleEqModel.store(0u, std::memory_order_relaxed);
    state.consoleLowDb.store(0.0f, std::memory_order_relaxed);
    state.consoleMidDb.store(0.0f, std::memory_order_relaxed);
    state.consoleAirDb.store(0.0f, std::memory_order_relaxed);
    state.dualTrackSafetyMode.store(false, std::memory_order_relaxed);
    state.mic2RoutingMode.store(0u, std::memory_order_relaxed);
    state.abLevelMatchedBypass.store(false, std::memory_order_relaxed);
    state.abAuditionMonitorOnly.store(true, std::memory_order_relaxed);
    state.ecoMode.store(false, std::memory_order_relaxed);
    state.freezeAbxGain.store(false, std::memory_order_relaxed);
    state.wdfEqEnabled.store(true, std::memory_order_relaxed);
    state.molecularClip.store(true, std::memory_order_relaxed);
    state.beautyAmount.store(0.0f, std::memory_order_relaxed);
    state.contourAmt.store(1.0f, std::memory_order_relaxed);
    state.harmonicSculptAmt.store(1.0f, std::memory_order_relaxed);
    state.deHarshAmt.store(1.0f, std::memory_order_relaxed);
    state.parallelGlueAmt.store(0.15f, std::memory_order_relaxed);
    state.backingHeadroomDb.store(-3.0f, std::memory_order_relaxed);
    state.voiceLoopState.store(0u, std::memory_order_relaxed);
    state.ultrasonicLpfEnabled.store(true, std::memory_order_relaxed);

    state.monitorReverbAmount.store(0.0f, std::memory_order_relaxed);
    state.crossfeedEnabled.store(false, std::memory_order_relaxed);
    state.cueCarveEnabled.store(false, std::memory_order_relaxed);

    state.driftAsrcEnabled.store(true, std::memory_order_relaxed);
    state.dacIspGuardEnabled.store(true, std::memory_order_relaxed);
    state.dacDitherShaperEnabled.store(true, std::memory_order_relaxed);

    state.learnState.store(static_cast<uint32_t>(LearnState::Idle), std::memory_order_relaxed);

    state.engineMode.store(static_cast<uint32_t>(EngineMode::WdmKsDirect), std::memory_order_relaxed);
    state.ksIrpCount.store(3, std::memory_order_relaxed);
    state.autopilotEnabled.store(true, std::memory_order_relaxed);
    state.requestedBufferSize.store(96, std::memory_order_relaxed);
    state.requestedSampleRate.store(48000.0, std::memory_order_relaxed);
    state.pCorePriority.store(true, std::memory_order_relaxed);
    state.adaptiveXRunGuard.store(true, std::memory_order_relaxed);
    state.configChangeCounter.store(0, std::memory_order_relaxed);

    state.themeMode.store(static_cast<uint32_t>(ThemeMode::Auto), std::memory_order_relaxed);

    state.deviceConnected.store(false, std::memory_order_relaxed);
    state.captureActive.store(false, std::memory_order_relaxed);
    state.streamRunning.store(false, std::memory_order_relaxed);
    state.activeEngineMode.store(static_cast<uint32_t>(EngineMode::WdmKsDirect), std::memory_order_relaxed);
    state.activeIrpCount.store(3, std::memory_order_relaxed);
    state.activeBufferSize.store(96, std::memory_order_relaxed);
    state.activeSampleRate.store(48000.0, std::memory_order_relaxed);
    state.activeBitDepth.store(24, std::memory_order_relaxed);
    state.inputChannels.store(2, std::memory_order_relaxed);
    state.outputChannels.store(2, std::memory_order_relaxed);
    state.inputLatencyMs.store(0.0f, std::memory_order_relaxed);
    state.outputLatencyMs.store(0.0f, std::memory_order_relaxed);
    state.roundTripLatencyMs.store(0.0f, std::memory_order_relaxed);

    state.hasHardwarePga.store(false, std::memory_order_relaxed);
    state.hardwarePgaMinDb.store(-63.5f, std::memory_order_relaxed);
    state.hardwarePgaMaxDb.store(0.0f, std::memory_order_relaxed);
    state.hasHardwareDac.store(false, std::memory_order_relaxed);
    state.hardwareDacMinDb.store(-63.5f, std::memory_order_relaxed);
    state.hardwareDacMaxDb.store(0.0f, std::memory_order_relaxed);
    state.hardwareMuted.store(false, std::memory_order_relaxed);
    state.captureMonoActive.store(false, std::memory_order_relaxed);
    state.supportsWaveRt.store(false, std::memory_order_relaxed);

    state.inputPeakDb.store(-96.0f, std::memory_order_relaxed);
    state.inputRmsDb.store(-96.0f, std::memory_order_relaxed);
    state.outputPeakDb.store(-96.0f, std::memory_order_relaxed);
    state.outputTruePeakDb.store(-96.0f, std::memory_order_relaxed);
    state.dacIspReductionDb.store(0.0f, std::memory_order_relaxed);
    state.gateGainReductionDb.store(0.0f, std::memory_order_relaxed);
    state.gateIsOpen.store(true, std::memory_order_relaxed);
    state.deEsserReductionDb.store(0.0f, std::memory_order_relaxed);
    state.plosiveReductionDb.store(0.0f, std::memory_order_relaxed);
    state.fetReductionDb.store(0.0f, std::memory_order_relaxed);
    state.levelerReductionDb.store(0.0f, std::memory_order_relaxed);
    state.adcClipDetected.store(false, std::memory_order_relaxed);
    state.learnProgress.store(0.0f, std::memory_order_relaxed);
    state.measuredNoiseFloorDb.store(-96.0f, std::memory_order_relaxed);
    state.preampCalibActive.store(false, std::memory_order_relaxed);
    state.recommendedPgaDb.store(0.0f, std::memory_order_relaxed);
    state.recommendedTrimDb.store(0.0f, std::memory_order_relaxed);
    state.abMatchGainDb.store(0.0f, std::memory_order_relaxed);
    state.dspCpuPercent.store(0.0f, std::memory_order_relaxed);
    state.capsuleCalibrated.store(false, std::memory_order_relaxed);
    state.clockDriftPpm.store(0.0f, std::memory_order_relaxed);

    state.callbackCpuPercent.store(0.0f, std::memory_order_relaxed);
    state.lastJitterUs.store(0.0f, std::memory_order_relaxed);
    state.maxJitterUs.store(0.0f, std::memory_order_relaxed);
    state.xrunCount.store(0, std::memory_order_relaxed);
    state.supportedSampleRatesMask.store(0b01011, std::memory_order_relaxed);
    state.jitterWriteIndex.store(0, std::memory_order_relaxed);

    for (auto& j : state.jitterHistoryUs) {
        j.store(0.0f, std::memory_order_relaxed);
    }
    state.deviceName[0] = '\0';
}

} // namespace stoneridge

#ifdef _MSC_VER
#pragma warning(pop)
#endif
