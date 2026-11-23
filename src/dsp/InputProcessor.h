#pragma once

#include "dsp/Em91cuCalibration.h"
#include "vocal/LoudnessMatcher.h"
#include "vocal/MicLocker.h"
#include "vocal/PreampAndConsole.h"
#include "vocal/VocalDynamics.h"

#include <array>
#include <cstdint>

namespace stoneridge {

struct InputDspParams {
    float gainDb               = 0.0f;
    bool  mute                 = false;
    bool  softClipEnabled      = true;
    bool  audioHpfEnabled      = false; // 80 Hz 2nd-order Butterworth rumble filter

    bool  gateEnabled          = false;
    bool  gateLookAheadEnabled = false; // 1.0 ms circular look-ahead (Off = zero latency)
    float gateThresholdDb      = -42.0f;
    float gateHysteresisDb     = 4.0f;
    float gateAttackMs         = 1.5f;
    float gateHoldMs           = 60.0f;
    float gateReleaseMs        = 140.0f;
    float gateRangeDb          = -60.0f;
    float sidechainHpfHz       = 90.0f;

    float humNotchFreqHz           = 0.0f;  // 0 (Off), 50, or 60 Hz (+ 2nd harmonic)
    bool  capsuleEqEnabled         = false; // EM-91CU+ voice contour EQ
    bool  capsuleCorrectionEnabled = false; // Pixel 11 Pro MEMS measured capsule correction
    float capsuleCorrectionStrength = 0.85f; // 0.0 to 1.0
    float deEsserAmount            = 0.0f;  // 0.0 to 1.0
    bool  plosiveGuardEnabled      = false; // Dynamic <140 Hz pop suppressor
    uint32_t micModel              = 0u;    // 0=Bypass, 1=C800G, 2=U87Ai, 3=251E, 4=U47, 5=SM7B
    float virtualDistanceCm        = 15.0f; // 5.0..35.0 cm spherical-wave proximity
    uint32_t preampModel           = 0u;    // 0=Bypass, 1=Neve1073, 2=API312, 3=V76, 4=SSL9000J
    float preampDriveDb            = 0.0f;  // 0.0 to 18.0 dB internal drive
    float transformerIronAmount    = 0.50f; // 0.0 to 1.0 Jiles-Atherton hysteresis mix
    float fetPeakCatchAmount       = 0.0f;  // 0.0 to 1.0 (1176 feedback FET peak catcher)
    uint32_t fetRatioMode          = 0u;    // 0 = 4:1, 1 = 8:1
    float vocalLevelerAmount       = 0.0f;  // 0.0 to 1.0 (LA-2A T4B dual-memory opto leveler)
    uint32_t consoleEqModel        = 0u;    // 0=Bypass, 1=Pultec, 2=1073, 3=SSL4000G, 4=API550A
    float consoleLowDb             = 0.0f;  // -6.0 to +6.0 dB
    float consoleMidDb             = 0.0f;  // -6.0 to +6.0 dB
    float consoleAirDb             = 0.0f;  // -6.0 to +8.0 dB
    bool  dualTrackSafetyMode      = false; // Legacy mirror of mic2RoutingMode == 2
    uint32_t mic2RoutingMode       = 0;     // 0=Mirror, 1=Pre-Dynamics Stem, 2=Raw Safety
    bool  abLevelMatchedBypass     = false; // ITU-R BS.1770 K-weighted level-matched raw A/B
    bool  abAuditionMonitorOnly    = false; // product default is true via SharedState
    bool  freezeAbxGain            = false;
    bool  ultrasonicLpfEnabled     = true;  // Auto 22 kHz 4th-order LPF at >=88.2 kHz
};

struct InputDspTelemetry {
    float peakDb              = -96.0f;
    float rmsDb               = -96.0f;
    float gateReductionDb     = 0.0f;
    bool  gateIsOpen          = true;
    float deEsserReductionDb  = 0.0f;
    float plosiveReductionDb  = 0.0f;
    float fetReductionDb      = 0.0f;
    float levelerReductionDb  = 0.0f;
    float abMatchGainDb       = 0.0f;
    bool  adcClipDetected     = false;
};

struct RoomNoiseResult {
    bool  completed           = false;
    float progress            = 0.0f;
    float noiseFloorDb        = -60.0f;
    float recommendedThreshDb = -42.0f;
    float recommendedHumHz    = 0.0f; // 0, 50, or 60
};

struct PreampAutoResult {
    bool  completed          = false;
    float progress           = 0.0f;
    float measuredPeakDb     = -96.0f;
    float recommendedPgaDb   = 0.0f;
    float recommendedTrimDb  = 0.0f;
};

// Zero-allocation, denormal-safe studio microphone capture & DSP processor.
class InputProcessor {
public:
    void Prepare(double sampleRate, uint32_t maxChannels) noexcept;
    void Reset() noexcept;

    void StartLearnRoomNoise() noexcept;
    bool IsLearningRoomNoise() const noexcept { return m_learnActive; }
    RoomNoiseResult GetLearnResult() const noexcept { return m_learnResult; }

    void StartPreampAutoLevel(float currentHardwarePgaDb, bool hasHardwarePga = true) noexcept;
    bool IsCalibratingPreamp() const noexcept { return m_preampCalibActive; }
    PreampAutoResult GetPreampAutoResult() const noexcept { return m_preampResult; }

    void ProcessStereo(float* left, float* right, uint32_t numFrames,
                       const InputDspParams& params, InputDspTelemetry& outTelemetry) noexcept;

    void ProcessMono(float* mono, uint32_t numFrames,
                     const InputDspParams& params, InputDspTelemetry& outTelemetry) noexcept;

    static void EnableDenormalProtection() noexcept;

private:
    struct Biquad {
        float lastParam = -1.0f;
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a1 = 0.0f, a2 = 0.0f;
        float z1[2] = {0.0f, 0.0f};
        float z2[2] = {0.0f, 0.0f};

        void Reset() noexcept {
            z1[0] = z1[1] = 0.0f;
            z2[0] = z2[1] = 0.0f;
        }
        void SetHpf(float cutoffHz, double sampleRate) noexcept;
        void SetNotch(float freqHz, float q, double sampleRate) noexcept;
        void SetBandpass(float freqHz, float q, double sampleRate) noexcept;
        void SetLowpass(float cutoffHz, double sampleRate) noexcept;
        void SetPeaking(float freqHz, float q, float gainDb, double sampleRate) noexcept;
        void SetLowShelf(float freqHz, float q, float gainDb, double sampleRate) noexcept;
        void SetHighShelf(float freqHz, float q, float gainDb, double sampleRate) noexcept;

        inline float Process(float x, uint32_t ch) noexcept {
            const float y = b0 * x + z1[ch];
            z1[ch] = b1 * x - a1 * y + z2[ch];
            z2[ch] = b2 * x - a2 * y;
            return y;
        }
    };

    // 1st-Order Polynomial ADAA Soft-Clipper state per channel
    struct AdaaClipper {
        float prevU[2] = {0.0f, 0.0f};
        void Reset() noexcept { prevU[0] = prevU[1] = 0.0f; }
        float Process(float x, uint32_t ch) noexcept;
    };

    void UpdateCachedParams(const InputDspParams& params) noexcept;
    void AccumulateLearnSample(float sample) noexcept;
    void AccumulatePreampSample(float sample) noexcept;

    double   m_sampleRate      = 48000.0;
    float    m_gainCoeff       = 0.004f;
    float    m_gainSmoothed    = 1.0f;
    float    m_gateEnv         = 1.0f;
    uint32_t m_holdCounter     = 0;
    bool     m_gateOpen        = true;

    // 5 Hz DC blocker state (always on)
    float    m_dcCoeff         = 0.9993f;
    float    m_dcPrevX[2]      = {0.0f, 0.0f};
    float    m_dcPrevY[2]      = {0.0f, 0.0f};

    // 1.0 ms circular look-ahead delay line (max 256 samples >= 192 @ 192kHz)
    static constexpr uint32_t kLookAheadMax = 256;
    std::array<float, kLookAheadMax> m_lookL{};
    std::array<float, kLookAheadMax> m_lookR{};
    uint32_t m_lookWriteIdx    = 0;
    uint32_t m_lookDelaySamples = 48;

    // Dynamic De-Esser, Plosive Guard & Opto Leveler envelopes
    float    m_deEssEnv        = 0.0f;
    float    m_deEssGain       = 1.0f;
    float    m_plosiveLowEnv   = 0.0f;
    float    m_plosiveMidEnv   = 0.0f;
    float    m_plosiveGain     = 1.0f;
    float    m_levelerRmsSq    = 0.0f;
    float    m_levelerGain     = 1.0f;

    float    m_envAtkFast      = 0.02f;
    float    m_envRelFast      = 0.001f;
    float    m_envRelSlow      = 0.0002f;

    // Cached parameters
    float    m_cachedGainDb    = -999.0f;
    bool     m_cachedMute      = false;
    float    m_targetGainLin   = 1.0f;
    float    m_cachedThreshDb  = -999.0f;
    float    m_cachedHystDb    = -999.0f;
    float    m_cachedRangeDb   = -999.0f;
    float    m_cachedAtkMs     = -999.0f;
    float    m_cachedHoldMs    = -999.0f;
    float    m_cachedRelMs     = -999.0f;

    float    m_openThreshLin   = 0.00794f;
    float    m_closeThreshLin  = 0.00501f;
    float    m_floorLin        = 0.001f;
    float    m_attackCoeff     = 0.01f;
    float    m_releaseCoeff    = 0.0001f;
    uint32_t m_holdSamples     = 2880;

    // Filters & ADAA clipper
    Biquad      m_audioHpf;
    Biquad      m_sidechainHpf;
    Biquad      m_humNotch1;
    Biquad      m_humNotch2;
    Biquad      m_eqDeMud;
    Biquad      m_eqDeHarsh;
    Biquad      m_eqAir;
    Biquad      m_deEssBp;
    Biquad      m_plosiveLp;
    Biquad      m_ultraLpf1;
    Biquad      m_ultraLpf2;
    std::array<Biquad, kEm91cuCalibSections> m_calibBiquads;
    float       m_cachedCalibStrength = -1.0f;
    AdaaClipper m_clipper;
    vocal::LoudnessMatcher   m_loudnessMatcher;
    vocal::MicLockerStage    m_micLocker;
    vocal::PreampStage       m_preampStage;
    vocal::Fet1176Stage      m_fet1176;
    vocal::OptoLa2aT4bStage  m_optoLa2a;
    vocal::ConsoleEqStage    m_consoleEq;
    std::array<float, 2048>  m_rawScratch{};

    // 2-second Room Noise & 50/60 Hz Goertzel calibrator
    bool            m_learnActive        = false;
    uint32_t        m_learnTargetSamples = 96000;
    uint32_t        m_learnCount         = 0;
    double          m_learnSumSq         = 0.0;
    float           m_learnPeakLin       = 0.0f;
    double          m_g50Coeff           = 0.0;
    double          m_g60Coeff           = 0.0;
    double          m_g50Q1 = 0.0, m_g50Q2 = 0.0;
    double          m_g60Q1 = 0.0, m_g60Q2 = 0.0;
    RoomNoiseResult m_learnResult{};

    // 3-second Preamp Sweet-Spot Auto-Level calibrator (-9 dBFS target)
    bool             m_preampCalibActive   = false;
    uint32_t         m_preampTargetSamples = 144000;
    uint32_t         m_preampCount         = 0;
    float            m_preampStartPgaDb    = 0.0f;
    bool             m_hasHwPga            = true;
    float            m_preampPeakLin       = 0.0f;
    PreampAutoResult m_preampResult{};
};

} // namespace stoneridge
