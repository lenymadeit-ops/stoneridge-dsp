#pragma once

#include <windows.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <immintrin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace stoneridge {

// AVX2 stereo peak detector (8 floats per iteration)
inline float ComputeStereoPeakAvx2(const float* l, const float* r, uint32_t frames) noexcept {
    const __m256 signMask = _mm256_set1_ps(-0.0f);
    __m256 maxVec = _mm256_setzero_ps();
    uint32_t i = 0;
    for (; i + 8 <= frames; i += 8) {
        const __m256 vl = _mm256_andnot_ps(signMask, _mm256_loadu_ps(l + i));
        const __m256 vr = _mm256_andnot_ps(signMask, _mm256_loadu_ps(r + i));
        maxVec = _mm256_max_ps(maxVec, _mm256_max_ps(vl, vr));
    }
    alignas(32) float tmp[8];
    _mm256_store_ps(tmp, maxVec);
    float peak = std::max({tmp[0], tmp[1], tmp[2], tmp[3], tmp[4], tmp[5], tmp[6], tmp[7]});
    for (; i < frames; ++i) {
        peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
    }
    return peak;
}

// AVX2 zero-latency direct monitor blend: dst[i] = clamp(fl[i] + mic[i] * mix, -1, 1)
inline void BlendDirectMonitorAvx2(const float* fl, const float* mic, float* dst,
                                   uint32_t frames, float mix) noexcept {
    const __m256 vMix = _mm256_set1_ps(mix);
    const __m256 vMin = _mm256_set1_ps(-1.0f);
    const __m256 vMax = _mm256_set1_ps(1.0f);
    uint32_t i = 0;
    for (; i + 8 <= frames; i += 8) {
        const __m256 vFl  = _mm256_loadu_ps(fl + i);
        const __m256 vMic = _mm256_loadu_ps(mic + i);
        const __m256 vSum = _mm256_fmadd_ps(vMic, vMix, vFl);
        _mm256_storeu_ps(dst + i, _mm256_min_ps(vMax, _mm256_max_ps(vMin, vSum)));
    }
    for (; i < frames; ++i) {
        dst[i] = std::clamp(fl[i] + mic[i] * mix, -1.0f, 1.0f);
    }
}

// Safe finite clamp (prevents NaN/Inf from plugins causing harsh digital rail pops)
inline float SafeClampSample(float x) noexcept {
    if (!(x == x)) return 0.0f;
    return std::clamp(x, -1.0f, 1.0f);
}

inline void ConvertPcmToFloat(const BYTE* src, float* dstL, float* dstR,
                              uint32_t frames, uint16_t channels, uint16_t bitsPerSample, bool isFloat) noexcept {
    if (isFloat && bitsPerSample == 32) {
        const float* f = reinterpret_cast<const float*>(src);
        for (uint32_t i = 0; i < frames; ++i) {
            dstL[i] = f[i * channels];
            dstR[i] = (channels > 1) ? f[i * channels + 1] : dstL[i];
        }
    } else if (bitsPerSample == 32 && channels == 2) {
        const int32_t* s = reinterpret_cast<const int32_t*>(src);
        const __m256 vScale = _mm256_set1_ps(1.0f / 2147483648.0f);
        uint32_t i = 0;
        for (; i + 4 <= frames; i += 4) {
            const __m256i raw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + i * 2));
            const __m256 flt  = _mm256_mul_ps(_mm256_cvtepi32_ps(raw), vScale);
            const __m128 lo   = _mm256_castps256_ps128(flt);
            const __m128 hi   = _mm256_extractf128_ps(flt, 1);
            _mm_storeu_ps(dstL + i, _mm_shuffle_ps(lo, hi, _MM_SHUFFLE(2, 0, 2, 0)));
            _mm_storeu_ps(dstR + i, _mm_shuffle_ps(lo, hi, _MM_SHUFFLE(3, 1, 3, 1)));
        }
        constexpr float kScale = 1.0f / 2147483648.0f;
        for (; i < frames; ++i) {
            dstL[i] = static_cast<float>(s[i * 2]) * kScale;
            dstR[i] = static_cast<float>(s[i * 2 + 1]) * kScale;
        }
    } else if (bitsPerSample == 32) {
        const int32_t* s = reinterpret_cast<const int32_t*>(src);
        constexpr float kScale = 1.0f / 2147483648.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            dstL[i] = static_cast<float>(s[i * channels]) * kScale;
            dstR[i] = dstL[i];
        }
    } else if (bitsPerSample == 24 && channels == 2) {
        // AVX2 pshufb fast path for 24-bit packed stereo PCM -> planar float32
        // Places 3 sample bytes into upper 24 bits of 32-bit int; scaling by 2^-31 divides out 2^8*2^23.
        const __m256i unpackMask = _mm256_set_epi8(
            11, 10, 9, -1,   8, 7, 6, -1,   5, 4, 3, -1,   2, 1, 0, -1,
            11, 10, 9, -1,   8, 7, 6, -1,   5, 4, 3, -1,   2, 1, 0, -1
        );
        const __m256 vScale = _mm256_set1_ps(1.0f / 2147483648.0f);
        uint32_t i = 0;
        for (; i + 6 <= frames; i += 4) {
            const __m128i lo128  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i * 6));
            const __m128i hi128  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i * 6 + 12));
            const __m256i raw    = _mm256_set_m128i(hi128, lo128);
            const __m256i s32    = _mm256_shuffle_epi8(raw, unpackMask);
            const __m256  flt    = _mm256_mul_ps(_mm256_cvtepi32_ps(s32), vScale);
            const __m128  loF    = _mm256_castps256_ps128(flt);
            const __m128  hiF    = _mm256_extractf128_ps(flt, 1);
            _mm_storeu_ps(dstL + i, _mm_shuffle_ps(loF, hiF, _MM_SHUFFLE(2, 0, 2, 0)));
            _mm_storeu_ps(dstR + i, _mm_shuffle_ps(loF, hiF, _MM_SHUFFLE(3, 1, 3, 1)));
        }
        constexpr float kScale = 1.0f / 8388608.0f;
        for (; i < frames; ++i) {
            const BYTE* pL = src + i * 6;
            const int32_t vL = static_cast<int32_t>(
                (static_cast<uint32_t>(pL[0]) << 8) |
                (static_cast<uint32_t>(pL[1]) << 16) |
                (static_cast<uint32_t>(pL[2]) << 24)) >> 8;
            dstL[i] = static_cast<float>(vL) * kScale;

            const BYTE* pR = pL + 3;
            const int32_t vR = static_cast<int32_t>(
                (static_cast<uint32_t>(pR[0]) << 8) |
                (static_cast<uint32_t>(pR[1]) << 16) |
                (static_cast<uint32_t>(pR[2]) << 24)) >> 8;
            dstR[i] = static_cast<float>(vR) * kScale;
        }
    } else if (bitsPerSample == 24) {
        constexpr float kScale = 1.0f / 8388608.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            const BYTE* pL = src + i * 3;
            const int32_t vL = static_cast<int32_t>(
                (static_cast<uint32_t>(pL[0]) << 8) |
                (static_cast<uint32_t>(pL[1]) << 16) |
                (static_cast<uint32_t>(pL[2]) << 24)) >> 8;
            dstL[i] = static_cast<float>(vL) * kScale;
            dstR[i] = dstL[i];
        }
    } else if (bitsPerSample == 16 && channels == 2) {
        const int16_t* s = reinterpret_cast<const int16_t*>(src);
        const __m256 vScale = _mm256_set1_ps(1.0f / 32768.0f);
        uint32_t i = 0;
        for (; i + 4 <= frames; i += 4) {
            const __m128i raw16 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i * 2));
            const __m256i raw32 = _mm256_cvtepi16_epi32(raw16);
            const __m256 flt    = _mm256_mul_ps(_mm256_cvtepi32_ps(raw32), vScale);
            const __m128 lo     = _mm256_castps256_ps128(flt);
            const __m128 hi     = _mm256_extractf128_ps(flt, 1);
            _mm_storeu_ps(dstL + i, _mm_shuffle_ps(lo, hi, _MM_SHUFFLE(2, 0, 2, 0)));
            _mm_storeu_ps(dstR + i, _mm_shuffle_ps(lo, hi, _MM_SHUFFLE(3, 1, 3, 1)));
        }
        constexpr float kScale = 1.0f / 32768.0f;
        for (; i < frames; ++i) {
            dstL[i] = static_cast<float>(s[i * 2]) * kScale;
            dstR[i] = static_cast<float>(s[i * 2 + 1]) * kScale;
        }
    } else {
        const int16_t* s = reinterpret_cast<const int16_t*>(src);
        constexpr float kScale = 1.0f / 32768.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            dstL[i] = static_cast<float>(s[i * channels]) * kScale;
            dstR[i] = (channels > 1) ? static_cast<float>(s[i * channels + 1]) * kScale : dstL[i];
        }
    }
}

inline void ConvertFloatToPcm(const float* srcL, const float* srcR, BYTE* dst,
                              uint32_t frames, uint16_t channels, uint16_t bitsPerSample, bool isFloat) noexcept {
    if (isFloat && bitsPerSample == 32) {
        float* f = reinterpret_cast<float*>(dst);
        for (uint32_t i = 0; i < frames; ++i) {
            f[i * channels] = SafeClampSample(srcL[i]);
            if (channels > 1) f[i * channels + 1] = SafeClampSample(srcR[i]);
        }
    } else if (bitsPerSample == 32 && channels == 2) {
        int32_t* d = reinterpret_cast<int32_t*>(dst);
        const __m128 vMin   = _mm_set1_ps(-1.0f);
        const __m128 vMax   = _mm_set1_ps(1.0f);
        const __m128 vScale = _mm_set1_ps(2147483520.0f);
        uint32_t i = 0;
        for (; i + 4 <= frames; i += 4) {
            const __m128 rawL = _mm_loadu_ps(srcL + i);
            const __m128 rawR = _mm_loadu_ps(srcR + i);
            const __m128 ordL = _mm_and_ps(rawL, _mm_cmp_ps(rawL, rawL, _CMP_ORD_Q));
            const __m128 ordR = _mm_and_ps(rawR, _mm_cmp_ps(rawR, rawR, _CMP_ORD_Q));
            const __m128 l = _mm_mul_ps(_mm_min_ps(vMax, _mm_max_ps(vMin, ordL)), vScale);
            const __m128 r = _mm_mul_ps(_mm_min_ps(vMax, _mm_max_ps(vMin, ordR)), vScale);
            const __m128i lo = _mm_cvtps_epi32(_mm_unpacklo_ps(l, r));
            const __m128i hi = _mm_cvtps_epi32(_mm_unpackhi_ps(l, r));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i * 2), lo);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i * 2 + 4), hi);
        }
        constexpr double kScale = 2147483647.0;
        for (; i < frames; ++i) {
            d[i * 2]     = static_cast<int32_t>(std::clamp(static_cast<double>(SafeClampSample(srcL[i])), -1.0, 1.0) * kScale);
            d[i * 2 + 1] = static_cast<int32_t>(std::clamp(static_cast<double>(SafeClampSample(srcR[i])), -1.0, 1.0) * kScale);
        }
    } else if (bitsPerSample == 32) {
        int32_t* d = reinterpret_cast<int32_t*>(dst);
        constexpr double kScale = 2147483647.0;
        for (uint32_t i = 0; i < frames; ++i) {
            d[i * channels] = static_cast<int32_t>(std::clamp(static_cast<double>(SafeClampSample(srcL[i])), -1.0, 1.0) * kScale);
        }
    } else if (bitsPerSample == 24 && channels == 2) {
        // AVX2 pshufb fast path for planar float32 -> 24-bit packed stereo PCM
        const __m128 vMin   = _mm_set1_ps(-1.0f);
        const __m128 vMax   = _mm_set1_ps(8388607.0f / 8388608.0f); // exact 24-bit +max saturation
        const __m128 vScale = _mm_set1_ps(2147483648.0f);
        const __m128i packMask = _mm_set_epi8(
            -1, -1, -1, -1,  15, 14, 13,  11, 10, 9,  7, 6, 5,  3, 2, 1
        );
        uint32_t i = 0;
        for (; i + 4 <= frames; i += 4) {
            const __m128 rawL = _mm_loadu_ps(srcL + i);
            const __m128 rawR = _mm_loadu_ps(srcR + i);
            // Zero out NaNs cleanly before clamping
            const __m128 ordL = _mm_and_ps(rawL, _mm_cmp_ps(rawL, rawL, _CMP_ORD_Q));
            const __m128 ordR = _mm_and_ps(rawR, _mm_cmp_ps(rawR, rawR, _CMP_ORD_Q));
            const __m128 clL  = _mm_mul_ps(_mm_min_ps(vMax, _mm_max_ps(vMin, ordL)), vScale);
            const __m128 clR  = _mm_mul_ps(_mm_min_ps(vMax, _mm_max_ps(vMin, ordR)), vScale);
            const __m128i lo32 = _mm_cvtps_epi32(_mm_unpacklo_ps(clL, clR)); // L0 R0 L1 R1
            const __m128i hi32 = _mm_cvtps_epi32(_mm_unpackhi_ps(clL, clR)); // L2 R2 L3 R3
            const __m128i pLo  = _mm_shuffle_epi8(lo32, packMask);
            const __m128i pHi  = _mm_shuffle_epi8(hi32, packMask);

            BYTE* outPtr = dst + i * 6;
            _mm_storel_epi64(reinterpret_cast<__m128i*>(outPtr), pLo);
            *reinterpret_cast<int32_t*>(outPtr + 8) = _mm_extract_epi32(pLo, 2);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(outPtr + 12), pHi);
            *reinterpret_cast<int32_t*>(outPtr + 20) = _mm_extract_epi32(pHi, 2);
        }
        constexpr float kScale = 8388608.0f;
        for (; i < frames; ++i) {
            const int32_t vL = std::clamp<int32_t>(static_cast<int32_t>(std::lrintf(SafeClampSample(srcL[i]) * kScale)), -8388608, 8388607);
            const int32_t vR = std::clamp<int32_t>(static_cast<int32_t>(std::lrintf(SafeClampSample(srcR[i]) * kScale)), -8388608, 8388607);
            BYTE* p = dst + i * 6;
            p[0] = static_cast<BYTE>(vL & 0xFF);
            p[1] = static_cast<BYTE>((vL >> 8) & 0xFF);
            p[2] = static_cast<BYTE>((vL >> 16) & 0xFF);
            p[3] = static_cast<BYTE>(vR & 0xFF);
            p[4] = static_cast<BYTE>((vR >> 8) & 0xFF);
            p[5] = static_cast<BYTE>((vR >> 16) & 0xFF);
        }
    } else if (bitsPerSample == 24) {
        constexpr float kScale = 8388608.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            const int32_t vL = std::clamp<int32_t>(static_cast<int32_t>(std::lrintf(SafeClampSample(srcL[i]) * kScale)), -8388608, 8388607);
            BYTE* p = dst + i * 3;
            p[0] = static_cast<BYTE>(vL & 0xFF);
            p[1] = static_cast<BYTE>((vL >> 8) & 0xFF);
            p[2] = static_cast<BYTE>((vL >> 16) & 0xFF);
        }
    } else if (bitsPerSample == 16 && channels == 2) {
        int16_t* d = reinterpret_cast<int16_t*>(dst);
        const __m128 vMin   = _mm_set1_ps(-1.0f);
        const __m128 vMax   = _mm_set1_ps(1.0f);
        const __m128 vScale = _mm_set1_ps(32768.0f);
        uint32_t i = 0;
        for (; i + 4 <= frames; i += 4) {
            const __m128 rawL = _mm_loadu_ps(srcL + i);
            const __m128 rawR = _mm_loadu_ps(srcR + i);
            const __m128 ordL = _mm_and_ps(rawL, _mm_cmp_ps(rawL, rawL, _CMP_ORD_Q));
            const __m128 ordR = _mm_and_ps(rawR, _mm_cmp_ps(rawR, rawR, _CMP_ORD_Q));
            const __m128 l = _mm_mul_ps(_mm_min_ps(vMax, _mm_max_ps(vMin, ordL)), vScale);
            const __m128 r = _mm_mul_ps(_mm_min_ps(vMax, _mm_max_ps(vMin, ordR)), vScale);
            const __m128i lo = _mm_cvtps_epi32(_mm_unpacklo_ps(l, r));
            const __m128i hi = _mm_cvtps_epi32(_mm_unpackhi_ps(l, r));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i * 2), _mm_packs_epi32(lo, hi));
        }
        constexpr float kScale = 32768.0f;
        for (; i < frames; ++i) {
            d[i * 2]     = static_cast<int16_t>(std::clamp<int32_t>(static_cast<int32_t>(std::lrintf(SafeClampSample(srcL[i]) * kScale)), -32768, 32767));
            d[i * 2 + 1] = static_cast<int16_t>(std::clamp<int32_t>(static_cast<int32_t>(std::lrintf(SafeClampSample(srcR[i]) * kScale)), -32768, 32767));
        }
    } else {
        int16_t* d = reinterpret_cast<int16_t*>(dst);
        constexpr float kScale = 32768.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            d[i * channels] = static_cast<int16_t>(std::clamp<int32_t>(static_cast<int32_t>(std::lrintf(SafeClampSample(srcL[i]) * kScale)), -32768, 32767));
            if (channels > 1) {
                d[i * channels + 1] = static_cast<int16_t>(std::clamp<int32_t>(static_cast<int32_t>(std::lrintf(SafeClampSample(srcR[i]) * kScale)), -32768, 32767));
            }
        }
    }
}

} // namespace stoneridge
