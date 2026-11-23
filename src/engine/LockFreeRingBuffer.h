#pragma once

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace stoneridge {

// Wait-free Single-Producer Single-Consumer (SPSC) Ring Buffer.
// Pre-allocates a power-of-two capacity on setup; zero allocations in push/pop.
template <typename T>
class LockFreeRingBuffer {
public:
    explicit LockFreeRingBuffer(size_t minCapacity = 4096) {
        Resize(minCapacity);
    }

    void Resize(size_t minCapacity) {
        size_t cap = 1;
        while (cap < minCapacity) {
            cap <<= 1;
        }
        m_mask = cap - 1;
        m_buffer.assign(cap, T{});
        m_writePos.store(0, std::memory_order_relaxed);
        m_readPos.store(0, std::memory_order_relaxed);
    }

    void Clear() noexcept {
        m_writePos.store(0, std::memory_order_relaxed);
        m_readPos.store(0, std::memory_order_relaxed);
    }

    size_t Capacity() const noexcept {
        return m_buffer.size();
    }

    size_t AvailableToRead() const noexcept {
        const size_t w = m_writePos.load(std::memory_order_acquire);
        const size_t r = m_readPos.load(std::memory_order_relaxed);
        return w - r;
    }

    size_t AvailableToWrite() const noexcept {
        const size_t w = m_writePos.load(std::memory_order_relaxed);
        const size_t r = m_readPos.load(std::memory_order_acquire);
        return m_buffer.size() - (w - r);
    }

    size_t Push(const T* data, size_t count) noexcept {
        const size_t w = m_writePos.load(std::memory_order_relaxed);
        const size_t r = m_readPos.load(std::memory_order_acquire);
        const size_t freeSpace = m_buffer.size() - (w - r);
        const size_t toWrite = std::min(count, freeSpace);
        if (toWrite == 0) {
            return 0;
        }

        const size_t idx = w & m_mask;
        const size_t firstPart = std::min(toWrite, m_buffer.size() - idx);
        std::memcpy(m_buffer.data() + idx, data, firstPart * sizeof(T));
        if (toWrite > firstPart) {
            std::memcpy(m_buffer.data(), data + firstPart, (toWrite - firstPart) * sizeof(T));
        }

        m_writePos.store(w + toWrite, std::memory_order_release);
        return toWrite;
    }

    size_t Pop(T* dest, size_t count) noexcept {
        const size_t w = m_writePos.load(std::memory_order_acquire);
        const size_t r = m_readPos.load(std::memory_order_relaxed);
        const size_t available = w - r;
        const size_t toRead = std::min(count, available);
        if (toRead == 0) {
            return 0;
        }

        const size_t idx = r & m_mask;
        const size_t firstPart = std::min(toRead, m_buffer.size() - idx);
        std::memcpy(dest, m_buffer.data() + idx, firstPart * sizeof(T));
        if (toRead > firstPart) {
            std::memcpy(dest + firstPart, m_buffer.data(), (toRead - firstPart) * sizeof(T));
        }

        m_readPos.store(r + toRead, std::memory_order_release);
        return toRead;
    }

    // Discard oldest samples to prevent capture-to-render latency creep
    size_t Drop(size_t count) noexcept {
        const size_t w = m_writePos.load(std::memory_order_acquire);
        const size_t r = m_readPos.load(std::memory_order_relaxed);
        const size_t toDrop = std::min(count, w - r);
        if (toDrop > 0) {
            m_readPos.store(r + toDrop, std::memory_order_release);
        }
        return toDrop;
    }

private:
    std::vector<T> m_buffer;
    size_t m_mask = 0;

    alignas(64) std::atomic<size_t> m_writePos{0};
    alignas(64) std::atomic<size_t> m_readPos{0};
};

} // namespace stoneridge

#ifdef _MSC_VER
#pragma warning(pop)
#endif
