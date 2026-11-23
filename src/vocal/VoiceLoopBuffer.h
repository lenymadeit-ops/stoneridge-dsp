#pragma once

// Fixed 5 s dry-vocal loop. Memory is static. Playback never allocates.

#include <algorithm>
#include <cstdint>

namespace stoneridge::vocal {

enum class VoiceLoopState : uint32_t {
    Idle = 0,
    Recording = 1,
    Looping = 2
};

class VoiceLoopBuffer {
public:
    static constexpr uint32_t kMaxSamples = 48000u * 5u;

    void Prepare(double sampleRate) noexcept {
        m_fs = (sampleRate > 1000.0) ? sampleRate : 48000.0;
        m_step = static_cast<float>(m_fs / 48000.0);
        if (m_step < 1.0f) m_step = 1.0f;
        Reset();
    }

    void Reset() noexcept {
        m_state = VoiceLoopState::Idle;
        m_count = 0;
        m_play = 0.0f;
        m_recAcc = 0.0f;
    }

    void Record() noexcept {
        m_state = VoiceLoopState::Recording;
        m_count = 0;
        m_recAcc = 0.0f;
    }

    void Play() noexcept {
        if (m_count > 32) {
            m_state = VoiceLoopState::Looping;
            m_play = 0.0f;
        }
    }

    void Stop() noexcept { m_state = VoiceLoopState::Idle; }

    VoiceLoopState State() const noexcept { return m_state; }
    uint32_t RecordedSamples() const noexcept { return m_count; }

    static float* Storage() noexcept {
        static float s_buf[kMaxSamples];
        return s_buf;
    }

    // Returns true when the loop replaces the dry input.
    inline bool Process(float& sample) noexcept {
        if (m_state == VoiceLoopState::Recording) {
            m_recAcc += 1.0f;
            if (m_recAcc >= m_step) {
                m_recAcc -= m_step;
                if (m_count < kMaxSamples) {
                    const float fade = (m_count < 480) ? (static_cast<float>(m_count) / 480.0f) : 1.0f;
                    Storage()[m_count++] = sample * fade;
                } else {
                    m_state = VoiceLoopState::Looping;
                    m_play = 0.0f;
                }
            }
            return false;
        }
        if (m_state == VoiceLoopState::Looping && m_count > 32) {
            const uint32_t i0 = static_cast<uint32_t>(m_play);
            const uint32_t i1 = (i0 + 1u) % m_count;
            const float frac = m_play - static_cast<float>(i0);
            sample = Storage()[i0] + (Storage()[i1] - Storage()[i0]) * frac;
            m_play += 1.0f / m_step;
            if (m_play >= static_cast<float>(m_count)) m_play -= static_cast<float>(m_count);
            return true;
        }
        return false;
    }

private:
    double m_fs = 48000.0;
    float m_step = 1.0f;
    float m_play = 0.0f;
    float m_recAcc = 0.0f;
    uint32_t m_count = 0;
    VoiceLoopState m_state = VoiceLoopState::Idle;
};

} // namespace stoneridge::vocal
