#pragma once

// AVX2 helpers and a 50 ms equal-power bypass crossfade. Scalar fallback is
// the same numeric path when AVX2 is compiled out.

#include <algorithm>
#include <cmath>
#include <cstdint>

#if defined(__AVX2__) || defined(_M_AVX2)
#include <immintrin.h>
#define STONERIDGE_HAVE_AVX2 1
#endif

namespace stoneridge::vocal {

inline float FastLog2(float x) noexcept {
    if (!(x > 0.0f)) return -32.0f;
    union { float f; uint32_t u; } v{ x };
    union { uint32_t u; float f; } mx{ (v.u & 0x007FFFFFu) | 0x3F000000u };
    const float y = static_cast<float>(v.u) * 1.1920928955078125e-7f;
    return y - 124.22551499f - 1.498030302f * mx.f - 1.72587999f / (0.3520887068f + mx.f);
}

inline float FastExp2(float x) noexcept {
    const int e = static_cast<int>(std::floor(x));
    const float f = x - static_cast<float>(e);
    const float m = 1.0f + f * (0.693147f + f * 0.240226f);
    union { float f; uint32_t u; } v{ m };
    v.u += static_cast<uint32_t>(e) << 23;
    return v.f;
}

class StageCrossfadeSleeper {
public:
    void Prepare(double sampleRate) noexcept {
        m_coeff = 1.0f - std::exp(-1.0f / (0.050f * static_cast<float>(std::max(1000.0, sampleRate))));
        Reset();
    }

    void Reset() noexcept { m_gain = 1.0f; }

    void SetEnabled(bool enabled) noexcept { m_target = enabled ? 1.0f : 0.0f; }

    bool IsAsleep() const noexcept { return m_gain < 1.0e-4f && m_target == 0.0f; }

    inline float Next() noexcept {
        m_gain += (m_target - m_gain) * m_coeff;
        return m_gain;
    }

private:
    float m_coeff = 0.001f;
    float m_gain = 1.0f;
    float m_target = 1.0f;
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
