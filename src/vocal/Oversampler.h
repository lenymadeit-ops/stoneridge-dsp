#pragma once

// Stoneridge Vocal Library - platform-free, header-only, zero-allocation.
// Shared by StoneridgeASIO (driver) and the future Stoneridge Vocal Chain VST3.
//
// Polyphase IIR half-band 2x / 4x oversampler (minimum-phase, zero added block latency).
// Design: two parallel chains of 1st-order allpass sections (in z^-1 at the low rate),
// coefficients from the elliptic half-band design of Valenzuela & Constantinides (1983),
// as popularised by Laurent de Soras' HIIR library (WTFPL). Coefficients are computed
// at Prepare() time on the non-RT thread; Process*() never allocates or locks.

#include <array>
#include <cmath>
#include <cstdint>

namespace stoneridge::vocal {

namespace detail {

inline double IPowP(double x, long n) noexcept {
    double z = 1.0;
    while (n != 0) {
        if ((n & 1) != 0) z *= x;
        n >>= 1;
        x *= x;
    }
    return z;
}

// Elliptic half-band design (Valenzuela & Constantinides). transition = normalized
// transition bandwidth (0 < tbw < 0.5, relative to the high sample rate / 2 ... i.e.
// passband edge at (0.5 - tbw) * fs_low / 2 * 2).
inline void ComputeHalfBandCoefs(double* coefs, int nbrCoefs, double transition) noexcept {
    constexpr double kPiD = 3.14159265358979323846;
    double k = std::tan((1.0 - transition * 2.0) * kPiD / 4.0);
    k *= k;
    const double kksqrt = std::pow(1.0 - k * k, 0.25);
    const double e  = 0.5 * (1.0 - kksqrt) / (1.0 + kksqrt);
    const double e2 = e * e;
    const double e4 = e2 * e2;
    const double q  = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));

    const int order = nbrCoefs * 2 + 1;
    for (int index = 0; index < nbrCoefs; ++index) {
        const int c = index + 1;
        // Numerator series
        double num = 0.0;
        {
            long i = 0;
            int j = 1;
            double term = 0.0;
            do {
                term = IPowP(q, i * (i + 1)) * std::sin(static_cast<double>(i * 2 + 1) * c * kPiD / order) * j;
                num += term;
                j = -j;
                ++i;
            } while (std::fabs(term) > 1e-100 && i < 64);
        }
        num *= std::pow(q, 0.25);
        // Denominator series
        double den = 0.0;
        {
            long i = 1;
            int j = -1;
            double term = 0.0;
            do {
                term = IPowP(q, i * i) * std::cos(static_cast<double>(i * 2) * c * kPiD / order) * j;
                den += term;
                j = -j;
                ++i;
            } while (std::fabs(term) > 1e-100 && i < 64);
        }
        den += 0.5;
        const double ww   = num / den;
        const double wwsq = ww * ww;
        const double x    = std::sqrt((1.0 - wwsq * k) * (1.0 - wwsq / k)) / (1.0 + wwsq);
        coefs[index] = (1.0 - x) / (1.0 + x);
    }
}

// One 2x half-band stage operating on a single channel.
template <int NCoefs>
class HalfBandStage {
public:
    static_assert(NCoefs >= 2 && NCoefs <= 16 && (NCoefs % 2) == 0, "even coefficient count required");

    void Design(double transition) noexcept {
        double c[NCoefs]{};
        ComputeHalfBandCoefs(c, NCoefs, transition);
        for (int i = 0; i < NCoefs; ++i) m_c[i] = static_cast<float>(c[i]);
        Reset();
    }

    void Reset() noexcept {
        m_xu.fill(0.0f); m_yu.fill(0.0f);
        m_xd.fill(0.0f); m_yd.fill(0.0f);
    }

    // Upsample one low-rate sample into two high-rate samples.
    inline void Up(float in, float& out0, float& out1) noexcept {
        float a = in; // even-coef chain
        float b = in; // odd-coef chain
        for (int s = 0; s < NCoefs; s += 2) {
            a = AllpassU(a, s);
            b = AllpassU(b, s + 1);
        }
        out0 = a;
        out1 = b;
    }

    // Downsample two high-rate samples into one low-rate sample.
    inline float Down(float in0, float in1) noexcept {
        float a = in1;
        float b = in0;
        for (int s = 0; s < NCoefs; s += 2) {
            a = AllpassD(a, s);
            b = AllpassD(b, s + 1);
        }
        return 0.5f * (a + b);
    }

private:
    // 1st-order allpass (c + z^-1) / (1 + c z^-1) at the low rate.
    inline float AllpassU(float x, int s) noexcept {
        const float y = m_c[s] * (x - m_yu[s]) + m_xu[s];
        m_xu[s] = x;
        m_yu[s] = y;
        return y;
    }
    inline float AllpassD(float x, int s) noexcept {
        const float y = m_c[s] * (x - m_yd[s]) + m_xd[s];
        m_xd[s] = x;
        m_yd[s] = y;
        return y;
    }

    float m_c[NCoefs]{};
    std::array<float, NCoefs> m_xu{}, m_yu{}, m_xd{}, m_yd{};
};

} // namespace detail

// 2x or 4x oversampler for one channel. Usage per block:
//   float* hi = os.Upsample(in, n);     // returns pointer to n*factor samples (internal buffer)
//   ... process hi[0 .. n*factor) in place ...
//   os.Downsample(out, n);              // writes n low-rate samples
// Blocks larger than kMaxBlock must be split by the caller.
class Oversampler {
public:
    static constexpr uint32_t kMaxBlock  = 2048;
    static constexpr uint32_t kMaxFactor = 4;

    void Prepare(uint32_t factor) noexcept {
        m_factor = (factor >= 4) ? 4u : (factor >= 2 ? 2u : 1u);
        // Stage 1 (base rate <-> 2x): steep, protects the audio band.
        m_s1.Design(0.0350);
        // Stage 2 (2x <-> 4x): only needs to reject images above the original Nyquist.
        m_s2.Design(0.2400);
        Reset();
    }

    void Reset() noexcept {
        m_s1.Reset();
        m_s2.Reset();
    }

    uint32_t Factor() const noexcept { return m_factor; }

    float* Upsample(const float* in, uint32_t n) noexcept {
        if (n > kMaxBlock) n = kMaxBlock;
        if (m_factor == 1) {
            for (uint32_t i = 0; i < n; ++i) m_buf[i] = in[i];
            return m_buf.data();
        }
        if (m_factor == 2) {
            for (uint32_t i = 0; i < n; ++i) m_s1.Up(in[i], m_buf[2 * i], m_buf[2 * i + 1]);
            return m_buf.data();
        }
        for (uint32_t i = 0; i < n; ++i) {
            float a = 0.0f, b = 0.0f;
            m_s1.Up(in[i], a, b);
            m_s2.Up(a, m_buf[4 * i + 0], m_buf[4 * i + 1]);
            m_s2.Up(b, m_buf[4 * i + 2], m_buf[4 * i + 3]);
        }
        return m_buf.data();
    }

    void Downsample(float* out, uint32_t n) noexcept {
        if (n > kMaxBlock) n = kMaxBlock;
        if (m_factor == 1) {
            for (uint32_t i = 0; i < n; ++i) out[i] = m_buf[i];
            return;
        }
        if (m_factor == 2) {
            for (uint32_t i = 0; i < n; ++i) out[i] = m_s1.Down(m_buf[2 * i], m_buf[2 * i + 1]);
            return;
        }
        for (uint32_t i = 0; i < n; ++i) {
            const float a = m_s2.Down(m_buf[4 * i + 0], m_buf[4 * i + 1]);
            const float b = m_s2.Down(m_buf[4 * i + 2], m_buf[4 * i + 3]);
            out[i] = m_s1.Down(a, b);
        }
    }

private:
    uint32_t m_factor = 2;
    detail::HalfBandStage<12> m_s1;
    detail::HalfBandStage<4>  m_s2;
    std::array<float, kMaxBlock * kMaxFactor> m_buf{};
};

} // namespace stoneridge::vocal
