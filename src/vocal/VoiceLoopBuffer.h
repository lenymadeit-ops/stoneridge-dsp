#pragma once

// Fixed 5 s dry-vocal loop. Memory is prepared on the non-RT thread; Process() never allocates.

#include "vocal/Oversampler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace stoneridge::vocal {

enum class VoiceLoopState : uint32_t {
    Idle = 0,
    Recording = 1,
    Looping = 2
};

class VoiceLoopBuffer {
public:
    static constexpr uint32_t kMaxDurationSec = 5u;

    void Prepare(double sampleRate) noexcept {
        m_fs = (sampleRate > 1000.0) ? sampleRate : 48000.0;
        m_decimate2x = (m_fs >= 176000.0);
        m_bufFs = m_decimate2x ? (m_fs * 0.5) : m_fs;

        const uint32_t bodyCap = std::clamp<uint32_t>(
            static_cast<uint32_t>(std::lround(m_bufFs * static_cast<double>(kMaxDurationSec))),
            48000u, 480000u);
        m_maxSamples = bodyCap;
        m_fadeSamples = std::clamp<uint32_t>(
            static_cast<uint32_t>(std::lround(m_bufFs * 0.010)), 16u, 1024u);
        m_buf.assign(static_cast<size_t>(m_maxSamples) + 4u, 0.0f);

        m_hbDown.Design(0.15);
        m_hbUp.Design(0.15);
        Reset();
    }

    void Reset() noexcept {
        m_state = VoiceLoopState::Idle;
        m_count = 0;
        m_playIdx = 0;
        m_firstPass = true;
        m_hbDown.Reset();
        m_hbUp.Reset();
        m_decimPhase = 0;
        m_decimPrev = 0.0f;
        m_upPhase = 0;
        m_upPair[0] = 0.0f;
        m_upPair[1] = 0.0f;
    }

    void Record() noexcept {
        if (m_buf.empty()) Prepare(48000.0);
        m_state = VoiceLoopState::Recording;
        m_count = 0;
        m_playIdx = 0;
        m_firstPass = true;
        m_hbDown.Reset();
        m_hbUp.Reset();
        m_decimPhase = 0;
        m_decimPrev = 0.0f;
    }

    void Play() noexcept {
        if (m_count > 32) {
            m_state = VoiceLoopState::Looping;
            m_playIdx = 0;
            m_firstPass = true;
            m_hbUp.Reset();
            m_upPhase = 0;
            m_upPair[0] = 0.0f;
            m_upPair[1] = 0.0f;
        }
    }

    void Stop() noexcept { m_state = VoiceLoopState::Idle; }

    VoiceLoopState State() const noexcept { return m_state; }
    uint32_t RecordedSamples() const noexcept { return m_count; }

    // Returns true when the loop replaces the dry input.
    inline bool Process(float& sample) noexcept {
        if (m_buf.empty()) return false;

        if (m_state == VoiceLoopState::Recording) {
            if (m_decimate2x) {
                if (m_decimPhase == 0) {
                    m_decimPrev = sample;
                    m_decimPhase = 1;
                    return false;
                }
                m_decimPhase = 0;
                const float filtered = m_hbDown.Down(m_decimPrev, sample);
                PushRecordedSample(filtered);
            } else {
                PushRecordedSample(sample);
            }
            return false;
        }

        if (m_state == VoiceLoopState::Looping && m_count > 32) {
            if (m_decimate2x) {
                if (m_upPhase == 0) {
                    const float base = NextLoopSample();
                    m_hbUp.Up(base, m_upPair[0], m_upPair[1]);
                    sample = m_upPair[0];
                    m_upPhase = 1;
                } else {
                    sample = m_upPair[1];
                    m_upPhase = 0;
                }
            } else {
                sample = NextLoopSample();
            }
            return true;
        }

        return false;
    }

private:
    inline void PushRecordedSample(float x) noexcept {
        if (m_count < m_maxSamples) {
            m_buf[m_count++] = x;
        } else {
            Play();
        }
    }

    inline float NextLoopSample() noexcept {
        const uint32_t count = m_count;
        const uint32_t fadeN = std::min<uint32_t>(m_fadeSamples, count / 4u);
        const uint32_t idx = m_playIdx % count;
        float y = m_buf[idx];

        if (m_firstPass && fadeN > 4u && idx < fadeN) {
            // Short soft start on very first playback pass only (buffer head itself stays un-faded).
            const uint32_t rampN = std::min<uint32_t>(fadeN, 96u);
            if (idx < rampN) {
                const float t = static_cast<float>(idx + 1u) / static_cast<float>(rampN);
                y *= 0.5f - 0.5f * std::cos(3.14159265f * t);
            }
        } else if (fadeN > 4u && idx + fadeN >= count) {
            // Equal-power cosine crossfade from loop tail directly into the un-faded head.
            const uint32_t k = idx - (count - fadeN);
            const float theta = 1.57079632f * (static_cast<float>(k + 1u) / static_cast<float>(fadeN + 1u));
            const float gTail = std::cos(theta);
            const float gHead = std::sin(theta);
            y = y * gTail + m_buf[k] * gHead;
        }

        ++m_playIdx;
        if (m_playIdx >= count) {
            m_firstPass = false;
            m_playIdx = (fadeN > 4u) ? fadeN : 0u;
        }
        return y;
    }

    double m_fs = 48000.0;
    double m_bufFs = 48000.0;
    bool m_decimate2x = false;
    uint32_t m_maxSamples = 48000u * 5u;
    uint32_t m_fadeSamples = 480u;
    uint32_t m_count = 0;
    uint32_t m_playIdx = 0;
    bool m_firstPass = true;
    VoiceLoopState m_state = VoiceLoopState::Idle;
    uint8_t m_decimPhase = 0;
    uint8_t m_upPhase = 0;
    float m_decimPrev = 0.0f;
    float m_upPair[2]{};
    detail::HalfBandStage<6> m_hbDown;
    detail::HalfBandStage<6> m_hbUp;
    std::vector<float> m_buf;
};

} // namespace stoneridge::vocal
