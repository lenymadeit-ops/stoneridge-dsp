#pragma once

// AVX2/SIMD math helpers, vectorized 8-channel Householder scatter, 4-biquad bank,
// dual half-band cascade, and exact 50 ms cosine equal-power bypass crossfade.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(__AVX2__) || defined(_M_AVX2)
#include <immintrin.h>
#define STONERIDGE_HAVE_AVX2 1
#else
#define STONERIDGE_HAVE_AVX2 0
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#endif
#endif

namespace stoneridge::vocal {

inline float FastLog2(float x) noexcept {
    if (!(x > 0.0f)) return -126.0f;
    uint32_t bits = 0;
    std::memcpy(&bits, &x, sizeof(bits));
    const int e = static_cast<int>((bits >> 23) & 0xFFu) - 127;
    const uint32_t mantBits = (bits & 0x007FFFFFu) | 0x3F800000u;
    float m = 1.0f;
    std::memcpy(&m, &mantBits, sizeof(m));
    // Centered at sqrt(2): u = (m - sqrt(2)) / (m + sqrt(2)) has |u| <= 0.171573, < 0.00002 dB error.
    constexpr float kSqrt2 = 1.41421356237309505f;
    constexpr float kC1 = 2.8853900817779268f;  // 2 / ln(2)
    constexpr float kC3 = 0.9617966939259756f;  // 2 / (3 * ln(2))
    constexpr float kC5 = 0.5770780163555854f;  // 2 / (5 * ln(2))
    const float u = (m - kSqrt2) / (m + kSqrt2);
    const float u2 = u * u;
    return static_cast<float>(e) + 0.5f + u * (kC1 + u2 * (kC3 + u2 * kC5));
}

inline float FastExp2(float x) noexcept {
    const float clamped = std::clamp(x, -126.0f, 126.0f);
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
    const int k = _mm_cvt_ss2si(_mm_set_ss(clamped));
#else
    const int k = static_cast<int>(clamped >= 0.0f ? (clamped + 0.5f) : (clamped - 0.5f));
#endif
    const float f = clamped - static_cast<float>(k);
    const float z = f * 0.6931471805599453f;
    const float poly = 1.0f + z * (1.0f + z * (0.5f + z * (0.16666667f + z * (0.041666668f + z * 0.008333334f))));
    const uint32_t scaleBits = static_cast<uint32_t>(k + 127) << 23;
    float scale = 1.0f;
    std::memcpy(&scale, &scaleBits, sizeof(scale));
    return poly * scale;
}

class StageCrossfadeSleeper {
public:
    void Prepare(double sampleRate) noexcept {
        const double sr = std::max(1000.0, sampleRate);
        m_step = static_cast<float>(1.0 / (0.050 * sr));
        ForceEnabled(true);
    }

    void Reset() noexcept {
        m_phase = m_target;
        m_gain = m_target;
    }

    void ForceEnabled(bool enabled) noexcept {
        m_target = enabled ? 1.0f : 0.0f;
        m_phase = m_target;
        m_gain = m_target;
    }

    void SetEnabled(bool enabled) noexcept { m_target = enabled ? 1.0f : 0.0f; }

    bool IsAsleep() const noexcept { return m_phase <= 0.0f && m_target == 0.0f; }

    float CurrentGain() const noexcept { return m_gain; }

    inline float Next() noexcept {
        if (m_phase == m_target) return m_gain;
        if (m_phase < m_target) {
            m_phase = std::min(1.0f, m_phase + m_step);
        } else {
            m_phase = std::max(0.0f, m_phase - m_step);
        }
        constexpr float kPi = 3.14159265358979323846f;
        m_gain = 0.5f - 0.5f * std::cos(kPi * m_phase);
        return m_gain;
    }

private:
    float m_step = 1.0f / 2400.0f;
    float m_phase = 1.0f;
    float m_gain = 1.0f;
    float m_target = 1.0f;
};

// Orthogonal 8x8 Householder feedback matrix: y[i] = x[i] - 0.25 * sum_{j=0..7} x[j].
inline void HouseholderScatter8(float v[8]) noexcept {
#if STONERIDGE_HAVE_AVX2
    const __m256 x = _mm256_loadu_ps(v);
    const __m128 hi = _mm256_extractf128_ps(x, 1);
    const __m128 lo = _mm256_castps256_ps128(x);
    __m128 s4 = _mm_add_ps(lo, hi);
    s4 = _mm_add_ps(s4, _mm_movehl_ps(s4, s4));
    s4 = _mm_add_ss(s4, _mm_shuffle_ps(s4, s4, 1));
    const __m256 negQuarterSum = _mm256_mul_ps(_mm256_set1_ps(_mm_cvtss_f32(s4)), _mm256_set1_ps(-0.25f));
    _mm256_storeu_ps(v, _mm256_add_ps(x, negQuarterSum));
#else
    const float sum = (v[0] + v[1]) + (v[2] + v[3]) + (v[4] + v[5]) + (v[6] + v[7]);
    const float sub = 0.25f * sum;
    for (int i = 0; i < 8; ++i) v[i] -= sub;
#endif
}

struct BiquadBank4 {
    float b0[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
    float b1[4]{};
    float b2[4]{};
    float a1[4]{};
    float a2[4]{};
    float z1[4]{};
    float z2[4]{};

    void Reset() noexcept {
        for (int i = 0; i < 4; ++i) z1[i] = z2[i] = 0.0f;
    }

    inline void Process4(const float in[4], float out[4]) noexcept {
#if STONERIDGE_HAVE_AVX2
        const __m128 vx = _mm_loadu_ps(in);
        const __m128 vb0 = _mm_loadu_ps(b0);
        const __m128 vb1 = _mm_loadu_ps(b1);
        const __m128 vb2 = _mm_loadu_ps(b2);
        const __m128 va1 = _mm_loadu_ps(a1);
        const __m128 va2 = _mm_loadu_ps(a2);
        const __m128 vz1 = _mm_loadu_ps(z1);
        const __m128 vz2 = _mm_loadu_ps(z2);
        const __m128 vy = _mm_fmadd_ps(vb0, vx, vz1);
        const __m128 nz1 = _mm_fnmadd_ps(va1, vy, _mm_fmadd_ps(vb1, vx, vz2));
        const __m128 nz2 = _mm_fnmadd_ps(va2, vy, _mm_mul_ps(vb2, vx));
        _mm_storeu_ps(z1, nz1);
        _mm_storeu_ps(z2, nz2);
        _mm_storeu_ps(out, vy);
#else
        for (int i = 0; i < 4; ++i) {
            const float x = in[i];
            const float y = b0[i] * x + z1[i];
            z1[i] = b1[i] * x - a1[i] * y + z2[i];
            z2[i] = b2[i] * x - a2[i] * y;
            out[i] = y;
        }
#endif
    }
};

// 4-lane HIIR allpass stage: processes [ch0_even, ch0_odd, ch1_even, ch1_odd] in parallel.
template <size_t HalfOrder>
struct DualHalfBandBranch {
    float cEven[HalfOrder]{};
    float cOdd[HalfOrder]{};
    float xu[HalfOrder][4]{};
    float yu[HalfOrder][4]{};
    float xd[HalfOrder][4]{};
    float yd[HalfOrder][4]{};

    void Reset() noexcept {
        for (size_t s = 0; s < HalfOrder; ++s) {
            for (int k = 0; k < 4; ++k) {
                xu[s][k] = yu[s][k] = 0.0f;
                xd[s][k] = yd[s][k] = 0.0f;
            }
        }
    }

    inline void UpDual(float inL, float inR, float& outL0, float& outL1, float& outR0, float& outR1) noexcept {
        alignas(16) float v[4] = { inL, inL, inR, inR };
        for (size_t s = 0; s < HalfOrder; ++s) {
            alignas(16) const float c[4] = { cEven[s], cOdd[s], cEven[s], cOdd[s] };
#if STONERIDGE_HAVE_AVX2
            const __m128 vx = _mm_loadu_ps(v);
            const __m128 vc = _mm_loadu_ps(c);
            const __m128 vx1 = _mm_loadu_ps(xu[s]);
            const __m128 vy1 = _mm_loadu_ps(yu[s]);
            const __m128 vy = _mm_fmadd_ps(vc, _mm_sub_ps(vx, vy1), vx1);
            _mm_storeu_ps(xu[s], vx);
            _mm_storeu_ps(yu[s], vy);
            _mm_storeu_ps(v, vy);
#else
            for (int k = 0; k < 4; ++k) {
                const float x = v[k];
                const float y = c[k] * (x - yu[s][k]) + xu[s][k];
                xu[s][k] = x;
                yu[s][k] = y;
                v[k] = y;
            }
#endif
        }
        outL0 = v[0]; outL1 = v[1];
        outR0 = v[2]; outR1 = v[3];
    }

    inline void DownDual(float inL0, float inL1, float inR0, float inR1, float& outL, float& outR) noexcept {
        alignas(16) float v[4] = { inL1, inL0, inR1, inR0 };
        for (size_t s = 0; s < HalfOrder; ++s) {
            alignas(16) const float c[4] = { cEven[s], cOdd[s], cEven[s], cOdd[s] };
#if STONERIDGE_HAVE_AVX2
            const __m128 vx = _mm_loadu_ps(v);
            const __m128 vc = _mm_loadu_ps(c);
            const __m128 vx1 = _mm_loadu_ps(xd[s]);
            const __m128 vy1 = _mm_loadu_ps(yd[s]);
            const __m128 vy = _mm_fmadd_ps(vc, _mm_sub_ps(vx, vy1), vx1);
            _mm_storeu_ps(xd[s], vx);
            _mm_storeu_ps(yd[s], vy);
            _mm_storeu_ps(v, vy);
#else
            for (int k = 0; k < 4; ++k) {
                const float x = v[k];
                const float y = c[k] * (x - yd[s][k]) + xd[s][k];
                xd[s][k] = x;
                yd[s][k] = y;
                v[k] = y;
            }
#endif
        }
        outL = 0.5f * (v[0] + v[1]);
        outR = 0.5f * (v[2] + v[3]);
    }
};

#if STONERIDGE_HAVE_AVX2
inline void FmaAdd8(float* dst, const float* a, const float* b, float scale) noexcept {
    const __m256 s = _mm256_set1_ps(scale);
    const __m256 va = _mm256_loadu_ps(a);
    const __m256 vb = _mm256_loadu_ps(b);
    _mm256_storeu_ps(dst, _mm256_fmadd_ps(vb, s, va));
}
#endif

} // namespace stoneridge::vocal
