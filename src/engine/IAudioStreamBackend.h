#pragma once

#include "asio/asio_abi.h"
#include "ipc/SharedState.h"

#include <cstdint>

namespace stoneridge {

class IAudioStreamBackend {
public:
    virtual ~IAudioStreamBackend() = default;

    virtual bool InitializeDevices(uint32_t sampleRate, uint32_t preferredBufferSize, SharedState* sharedState) = 0;
    virtual void ShutdownDevices() = 0;

    virtual bool CreateBuffers(uint32_t bufferSize,
                               uint32_t activeInputMask,
                               uint32_t activeOutputMask,
                               ASIOCallbacks* callbacks) = 0;
    virtual void DisposeBuffers() = 0;

    virtual bool Start() = 0;
    virtual void Stop() = 0;

    virtual uint32_t GetBufferSize() const noexcept = 0;
    virtual uint32_t GetPreferredBufferSize() const noexcept = 0;
    virtual uint32_t GetMinBufferSize() const noexcept = 0;
    virtual uint32_t GetMaxBufferSize() const noexcept = 0;
    virtual long     GetBufferGranularity() const noexcept = 0;

    virtual double   GetSampleRate() const noexcept = 0;
    virtual uint32_t GetInputChannels() const noexcept = 0;
    virtual uint32_t GetOutputChannels() const noexcept = 0;
    virtual uint32_t GetInputLatencySamples() const noexcept = 0;
    virtual uint32_t GetOutputLatencySamples() const noexcept = 0;
    virtual uint32_t GetSupportedSampleRatesMask() const noexcept = 0;

    virtual void* GetAsioBufferPtr(bool isInput, uint32_t channel, uint32_t doubleBufferIndex) noexcept = 0;
    virtual void  GetSamplePosition(uint64_t& outSamples, uint64_t& outSystemTimeNs) const noexcept = 0;
    virtual void  SyncHardwareControls() {}
};

} // namespace stoneridge
