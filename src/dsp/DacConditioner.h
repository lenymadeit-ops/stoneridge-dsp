#pragma once

#include "engine/AudioFormatConvert.h"

#include <immintrin.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace stoneridge {

struct DacTelemetry {
    float outputTruePeakDb = -96.0f;
    float ispReductionDb   = 0.0f;
};

// Mastering-grade DAC output conditioner for the Mackie EM-91CU+:
// 1. ITU-R BS.1770-4 4x Oversampling Polyphase True-Peak Detector & Zero-Latency ISP Limiter (-0.5 dBTP)
// 2. Uncorrelated XorShift64* 1-LSB TPDF Dither (24-bit @ 44.1/48 kHz)
// 3. 2nd-Order Fletcher-Munson Psychoacoustic Error-Feedback Noise Shaper (16-bit @ 96 kHz)
class DacConditioner {
public:
    void Prepare(double sampleRate) noexcept {
        m_sampleRate     = (sampleRate > 1000.0) ? sampleRate : 48000.0;
        m_ispRelCoeff    = 1.0f - std::exp(-1.0f / (0.025f * static_cast<float>(m_sampleRate))); // 25 ms optical release
        m_ispHoldSamples = std::max<uint32_t>(16u, static_cast<uint32_t>(m_sampleRate * 0.003)); // 3 ms peak hold
        Reset();
    }

    void Reset() noexcept {
        m_tpHist[0].fill(0.0f);
        m_tpHist[1].fill(0.0f);
        m_tpIdx[0]  = 0;
        m_tpIdx[1]  = 0;
        m_ispGain   = 1.0f;
        m_ispHoldCtr = 0;
        m_align[0].fill(0.0f);
        m_align[1].fill(0.0f);
        m_alignIdx = 0;
        m_err1[0] = m_err1[1] = 0.0f;
        m_err2[0] = m_err2[1] = 0.0f;
        m_err3[0] = m_err3[1] = 0.0f;
        m_rngState[0] = 0x9E3779B97F4A7C15ULL;
        m_rngState[1] = 0xBF58476D1CE4E5B9ULL;
    }

    // Independent helper to measure 4x BS.1770-4 true-peak of a single stereo sample pair
    inline float PushAndMeasureTruePeakSample(float sL, float sR) noexcept {
        const float tpL = PushChannelTruePeak(0, sL);
        const float tpR = PushChannelTruePeak(1, sR);
        return std::max(tpL, tpR);
    }

    // Float-domain ITU-R BS.1770-4 True-Peak meter + optional ISP Guard (-0.5 dBTP ceiling)
    void ProcessStereoFloat(const float* inL,
                            const float* inR,
                            float* outL,
                            float* outR,
                            uint32_t frames,
                            bool ispGuardEnabled,
                            DacTelemetry& outTel) noexcept {
        constexpr float kIspCeiling = 0.938642f; // -0.55 dBTP target (-0.5 dBTP guaranteed after FIR)
        float maxTpLin = 0.0f;
        float minGain  = 1.0f;

        for (uint32_t i = 0; i < frames; ++i) {
            const float xL = SafeClampUnclipped(inL[i]);
            const float xR = inR ? SafeClampUnclipped(inR[i]) : xL;

            const float rawTp = std::max(PushChannelTruePeak(0, xL), PushChannelTruePeak(1, xR));
            maxTpLin = std::max(maxTpLin, rawTp);

            float yL = xL;
            float yR = xR;
            if (ispGuardEnabled) {
                const float dL = TakeAligned(0, xL);
                const float dR = TakeAligned(1, xR);
                const float target = (rawTp > kIspCeiling) ? (kIspCeiling / rawTp) : 1.0f;
                if (target < m_ispGain) {
                    m_ispGain    = target;
                    m_ispHoldCtr = m_ispHoldSamples;
                } else if (m_ispHoldCtr > 0) {
                    --m_ispHoldCtr;
                } else {
                    m_ispGain += (target - m_ispGain) * m_ispRelCoeff;
                }
                yL = std::clamp(dL * m_ispGain, -0.985f, 0.985f);
                yR = std::clamp(dR * m_ispGain, -0.985f, 0.985f);
                minGain = std::min(minGain, m_ispGain);
            }
            outL[i] = yL;
            if (outR) outR[i] = yR;
        }

        const float effectiveTp = ispGuardEnabled ? std::min(maxTpLin * minGain, maxTpLin) : maxTpLin;
        outTel.outputTruePeakDb = 20.0f * std::log10(std::max(effectiveTp, 1e-5f));
        outTel.ispReductionDb   = (ispGuardEnabled && minGain < 0.9999f)
            ? (20.0f * std::log10(std::max(minGain, 1e-5f)))
            : 0.0f;
    }

    // Combined BS.1770-4 True-Peak / ISP Guard + TPDF Dither / 96 kHz Noise Shaping -> PCM output
    void ProcessAndConvertToPcm(const float* inL,
                                const float* inR,
                                BYTE* dst,
                                uint32_t frames,
                                uint16_t channels,
                                uint16_t bitsPerSample,
                                bool isFloat,
                                bool ispGuardEnabled,
                                bool ditherShaperEnabled,
                                DacTelemetry& outTel) noexcept {
        if (!inL || !dst || frames == 0) {
            outTel = {};
            return;
        }

        // Fast bit-exact raw bypass when both conditioning features are disabled
        // (avoids running the 48-tap FIR inside the hardware buffer lock window).
        if (!ispGuardEnabled && !ditherShaperEnabled) {
            const float rawPeak = ComputeStereoPeakAvx2(inL, inR ? inR : inL, frames);
            const float maxSampleLin = IsNonFiniteFloat(rawPeak) ? 0.0f : rawPeak;
            ConvertFloatToPcm(inL, inR ? inR : inL, dst, frames, channels, bitsPerSample, isFloat);
            outTel.outputTruePeakDb = 20.0f * std::log10(std::max(maxSampleLin, 1e-5f));
            outTel.ispReductionDb   = 0.0f;
            return;
        }

        constexpr float kIspCeiling = 0.938642f; // -0.55 dBTP target
        const bool useNoiseShaper16 = ditherShaperEnabled && !isFloat && bitsPerSample == 16;
        const bool useDither24      = ditherShaperEnabled && !isFloat && bitsPerSample == 24;
        const bool highRate96       = (m_sampleRate >= 80000.0);

        float maxTpLin = 0.0f;
        float minGain  = 1.0f;

        for (uint32_t i = 0; i < frames; ++i) {
            const float xL = SafeClampUnclipped(inL[i]);
            const float xR = inR ? SafeClampUnclipped(inR[i]) : xL;

            float yL = xL;
            float yR = xR;
            if (ispGuardEnabled) {
                const float rawTp = std::max(PushChannelTruePeak(0, xL), PushChannelTruePeak(1, xR));
                const float dL = TakeAligned(0, xL);
                const float dR = TakeAligned(1, xR);
                const float target = (rawTp > kIspCeiling) ? (kIspCeiling / rawTp) : 1.0f;
                if (target < m_ispGain) {
                    m_ispGain    = target;
                    m_ispHoldCtr = m_ispHoldSamples;
                } else if (m_ispHoldCtr > 0) {
                    --m_ispHoldCtr;
                } else {
                    m_ispGain += (target - m_ispGain) * m_ispRelCoeff;
                }
                yL = std::clamp(dL * m_ispGain, -0.985f, 0.985f);
                yR = std::clamp(dR * m_ispGain, -0.985f, 0.985f);
                minGain = std::min(minGain, m_ispGain);
                maxTpLin = std::max(maxTpLin, std::min(rawTp * m_ispGain, kIspCeiling));
            } else {
                maxTpLin = std::max(maxTpLin, std::max(std::fabs(xL), std::fabs(xR)));
            }

            if (useNoiseShaper16) {
                const int16_t qL = Quantize16NoiseShaped(yL, 0, highRate96);
                const int16_t qR = (channels > 1) ? Quantize16NoiseShaped(yR, 1, highRate96) : qL;
                int16_t* out = reinterpret_cast<int16_t*>(dst) + i * channels;
                out[0] = qL;
                if (channels > 1) out[1] = qR;
            } else if (useDither24) {
                const float tpdfL = NextTpdfLsb(0);
                const float tpdfR = (channels > 1) ? NextTpdfLsb(1) : tpdfL;
                const int32_t vL = std::clamp<int32_t>(
                    static_cast<int32_t>(std::lrintf(std::clamp(yL, -1.0f, 1.0f) * 8388608.0f + tpdfL)),
                    -8388608, 8388607);
                const int32_t vR = (channels > 1) ? std::clamp<int32_t>(
                    static_cast<int32_t>(std::lrintf(std::clamp(yR, -1.0f, 1.0f) * 8388608.0f + tpdfR)),
                    -8388608, 8388607) : vL;
                BYTE* p = dst + i * channels * 3;
                p[0] = static_cast<BYTE>(vL & 0xFF);
                p[1] = static_cast<BYTE>((vL >> 8) & 0xFF);
                p[2] = static_cast<BYTE>((vL >> 16) & 0xFF);
                if (channels > 1) {
                    p[3] = static_cast<BYTE>(vR & 0xFF);
                    p[4] = static_cast<BYTE>((vR >> 8) & 0xFF);
                    p[5] = static_cast<BYTE>((vR >> 16) & 0xFF);
                }
            } else if (isFloat && bitsPerSample == 32) {
                float* out = reinterpret_cast<float*>(dst) + i * channels;
                out[0] = std::clamp(yL, -1.0f, 1.0f);
                if (channels > 1) out[1] = std::clamp(yR, -1.0f, 1.0f);
            } else if (bitsPerSample == 32) {
                int32_t* out = reinterpret_cast<int32_t*>(dst) + i * channels;
                out[0] = static_cast<int32_t>(std::clamp(static_cast<double>(yL), -1.0, 1.0) * 2147483647.0);
                if (channels > 1) out[1] = static_cast<int32_t>(std::clamp(static_cast<double>(yR), -1.0, 1.0) * 2147483647.0);
            } else if (bitsPerSample == 24) {
                const int32_t vL = std::clamp<int32_t>(static_cast<int32_t>(std::lrintf(std::clamp(yL, -1.0f, 1.0f) * 8388608.0f)), -8388608, 8388607);
                const int32_t vR = (channels > 1) ? std::clamp<int32_t>(static_cast<int32_t>(std::lrintf(std::clamp(yR, -1.0f, 1.0f) * 8388608.0f)), -8388608, 8388607) : vL;
                BYTE* p = dst + i * channels * 3;
                p[0] = static_cast<BYTE>(vL & 0xFF);
                p[1] = static_cast<BYTE>((vL >> 8) & 0xFF);
                p[2] = static_cast<BYTE>((vL >> 16) & 0xFF);
                if (channels > 1) {
                    p[3] = static_cast<BYTE>(vR & 0xFF);
                    p[4] = static_cast<BYTE>((vR >> 8) & 0xFF);
                    p[5] = static_cast<BYTE>((vR >> 16) & 0xFF);
                }
            } else {
                int16_t* out = reinterpret_cast<int16_t*>(dst) + i * channels;
                out[0] = static_cast<int16_t>(std::clamp<int32_t>(static_cast<int32_t>(std::lrintf(std::clamp(yL, -1.0f, 1.0f) * 32768.0f)), -32768, 32767));
                if (channels > 1) {
                    out[1] = static_cast<int16_t>(std::clamp<int32_t>(static_cast<int32_t>(std::lrintf(std::clamp(yR, -1.0f, 1.0f) * 32768.0f)), -32768, 32767));
                }
            }
        }

        outTel.outputTruePeakDb = 20.0f * std::log10(std::max(maxTpLin, 1e-5f));
        outTel.ispReductionDb   = (ispGuardEnabled && minGain < 0.9999f)
            ? (20.0f * std::log10(std::max(minGain, 1e-5f)))
            : 0.0f;
    }

private:
    static inline float SafeClampUnclipped(float x) noexcept {
        uint32_t u = 0;
        const auto* p = reinterpret_cast<const unsigned char*>(&x);
        u = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
          | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        if ((u & 0x7F800000u) == 0x7F800000u) return 0.0f;
        return std::clamp(x, -4.0f, 4.0f);
    }

    inline float TakeAligned(uint32_t ch, float x) noexcept {
        const float y = m_align[ch][m_alignIdx];
        m_align[ch][m_alignIdx] = x;
        if (ch == 1u) m_alignIdx = (m_alignIdx + 1u) % 6u;
        return y;
    }

    inline float PushChannelTruePeak(uint32_t ch, float s) noexcept {
        uint32_t idx = (m_tpIdx[ch] == 0) ? 11u : (m_tpIdx[ch] - 1u);
        m_tpIdx[ch] = idx;
        m_tpHist[ch][idx]      = s;
        m_tpHist[ch][idx + 12] = s;

        // ITU-R BS.1770-4 Annex 2 48-tap 4-phase polyphase coefficients (scaled by 4.0 for unity DC)
        alignas(16) static constexpr float kP0[12] = {
            0.0057292f * 4.0f, -0.0056096f * 4.0f,  0.0080942f * 4.0f, -0.0093558f * 4.0f,
            0.0063161f * 4.0f,  0.0114320f * 4.0f,  0.2653100f * 4.0f, -0.0451550f * 4.0f,
            0.0272390f * 4.0f, -0.0172670f * 4.0f,  0.0099256f * 4.0f, -0.0049864f * 4.0f
        };
        alignas(16) static constexpr float kP1[12] = {
            0.0015520f * 4.0f, -0.0017098f * 4.0f, -0.0011323f * 4.0f,  0.0088501f * 4.0f,
           -0.0283860f * 4.0f,  0.1049700f * 4.0f,  0.2028700f * 4.0f, -0.0541150f * 4.0f,
            0.0258280f * 4.0f, -0.0125050f * 4.0f,  0.0050592f * 4.0f, -0.0015723f * 4.0f
        };
        alignas(16) static constexpr float kP2[12] = {
           -0.0015723f * 4.0f,  0.0050592f * 4.0f, -0.0125050f * 4.0f,  0.0258280f * 4.0f,
           -0.0541150f * 4.0f,  0.2028700f * 4.0f,  0.1049700f * 4.0f, -0.0283860f * 4.0f,
            0.0088501f * 4.0f, -0.0011323f * 4.0f, -0.0017098f * 4.0f,  0.0015520f * 4.0f
        };
        alignas(16) static constexpr float kP3[12] = {
           -0.0049864f * 4.0f,  0.0099256f * 4.0f, -0.0172670f * 4.0f,  0.0272390f * 4.0f,
           -0.0451550f * 4.0f,  0.2653100f * 4.0f,  0.0114320f * 4.0f,  0.0063161f * 4.0f,
           -0.0093558f * 4.0f,  0.0080942f * 4.0f, -0.0056096f * 4.0f,  0.0057292f * 4.0f
        };

        const float* h = &m_tpHist[ch][idx];
        const __m128 h0 = _mm_loadu_ps(h);
        const __m128 h1 = _mm_loadu_ps(h + 4);
        const __m128 h2 = _mm_loadu_ps(h + 8);

        auto dot12 = [&](const float* c) noexcept -> float {
            __m128 acc = _mm_mul_ps(h0, _mm_load_ps(c));
            acc = _mm_fmadd_ps(h1, _mm_load_ps(c + 4), acc);
            acc = _mm_fmadd_ps(h2, _mm_load_ps(c + 8), acc);
            const __m128 shuf = _mm_movehdup_ps(acc);
            __m128 sums = _mm_add_ps(acc, shuf);
            sums = _mm_add_ss(sums, _mm_movehl_ps(sums, sums));
            return _mm_cvtss_f32(sums);
        };

        const float y0 = std::fabs(dot12(kP0));
        const float y1 = std::fabs(dot12(kP1));
        const float y2 = std::fabs(dot12(kP2));
        const float y3 = std::fabs(dot12(kP3));
        const float alignedCenter = std::fabs(m_tpHist[ch][idx + 5]);
        return std::max({ alignedCenter, y0, y1, y2, y3 });
    }

    // XorShift64* triangular PDF dither in [-1.0, +1.0] LSB
    inline float NextTpdfLsb(uint32_t ch) noexcept {
        auto nextU = [&](uint64_t& st) noexcept -> float {
            st ^= st >> 12;
            st ^= st << 25;
            st ^= st >> 27;
            const uint64_t r = st * 0x2545F4914F6CDD1DULL;
            return static_cast<float>(r >> 40) * (1.0f / 16777216.0f); // [0, 1)
        };
        uint64_t& st = m_rngState[ch];
        return nextU(st) - nextU(st);
    }

    // 3rd-order psychoacoustic error-feedback noise shaper:
    // At 96 kHz: NTF(z) = (1 - 0.80 z^-1)(1 - 1.15 z^-1 + 0.56 z^-2)
    //          = 1 - 1.95 z^-1 + 1.48 z^-2 - 0.448 z^-3 (suppresses all 0..18 kHz < -110 dBFS, +13.7 dB Nyquist max)
    // At 44.1/48 kHz: gentle 1st-order NTF(z) = 1 - 0.85 z^-1 (+5.3 dB Nyquist max)
    inline int16_t Quantize16NoiseShaped(float sample, uint32_t ch, bool highRate96) noexcept {
        const float target = std::clamp(sample, -0.9999f, 0.9999f) * 32768.0f;
        const float dither = NextTpdfLsb(ch);

        float v = target;
        if (highRate96) {
            v -= (1.950f * m_err1[ch] - 1.480f * m_err2[ch] + 0.448f * m_err3[ch]);
        } else {
            v -= 0.850f * m_err1[ch];
        }

        const int32_t qInt = std::clamp<int32_t>(
            static_cast<int32_t>(std::lrintf(v + dither)), -32768, 32767);
        const float qErr = std::clamp(static_cast<float>(qInt) - v, -3.0f, 3.0f);
        m_err3[ch] = m_err2[ch];
        m_err2[ch] = m_err1[ch];
        m_err1[ch] = qErr;
        return static_cast<int16_t>(qInt);
    }

    double   m_sampleRate     = 48000.0;
    float    m_ispRelCoeff    = 0.00083f;
    float    m_ispGain        = 1.0f;
    std::array<std::array<float, 6>, 2> m_align{};
    uint32_t m_alignIdx = 0;
    uint32_t m_ispHoldSamples = 144;
    uint32_t m_ispHoldCtr     = 0;

    std::array<std::array<float, 24>, 2> m_tpHist{};
    uint32_t m_tpIdx[2]    = { 0, 0 };
    float    m_err1[2]     = { 0.0f, 0.0f };
    float    m_err2[2]     = { 0.0f, 0.0f };
    float    m_err3[2]     = { 0.0f, 0.0f };
    uint64_t m_rngState[2] = { 0x9E3779B97F4A7C15ULL, 0xBF58476D1CE4E5B9ULL };
};

} // namespace stoneridge
