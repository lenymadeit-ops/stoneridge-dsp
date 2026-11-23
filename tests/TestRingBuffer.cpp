#include <catch2/catch_test_macros.hpp>
#include "engine/LockFreeRingBuffer.h"

#include <numeric>
#include <thread>
#include <vector>

TEST_CASE("LockFreeRingBuffer basic push and pop", "[ringbuffer]") {
    stoneridge::LockFreeRingBuffer<float> rb(16);
    REQUIRE(rb.Capacity() >= 16);
    REQUIRE(rb.AvailableToRead() == 0);

    std::vector<float> in(10);
    std::iota(in.begin(), in.end(), 1.0f);

    REQUIRE(rb.Push(in.data(), in.size()) == 10);
    REQUIRE(rb.AvailableToRead() == 10);

    std::vector<float> out(10, 0.0f);
    REQUIRE(rb.Pop(out.data(), out.size()) == 10);
    REQUIRE(out == in);
    REQUIRE(rb.AvailableToRead() == 0);
}

TEST_CASE("LockFreeRingBuffer wrap-around and SPSC concurrency", "[ringbuffer]") {
    stoneridge::LockFreeRingBuffer<uint32_t> rb(64);
    constexpr uint32_t kTotalItems = 50000;

    std::thread producer([&]() {
        uint32_t next = 0;
        while (next < kTotalItems) {
            uint32_t batch[7];
            const size_t count = std::min<size_t>(7, kTotalItems - next);
            for (size_t i = 0; i < count; ++i) {
                batch[i] = next + static_cast<uint32_t>(i);
            }
            const size_t pushed = rb.Push(batch, count);
            next += static_cast<uint32_t>(pushed);
        }
    });

    std::vector<uint32_t> received;
    received.reserve(kTotalItems);
    std::thread consumer([&]() {
        uint32_t buf[5];
        while (received.size() < kTotalItems) {
            const size_t popped = rb.Pop(buf, 5);
            for (size_t i = 0; i < popped; ++i) {
                received.push_back(buf[i]);
            }
        }
    });

    producer.join();
    consumer.join();

    REQUIRE(received.size() == kTotalItems);
    for (uint32_t i = 0; i < kTotalItems; ++i) {
        REQUIRE(received[i] == i);
    }
}
