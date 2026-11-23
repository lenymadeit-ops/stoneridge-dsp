#pragma once

#include <unknwn.h>
#include <cstdint>

// Clean-room binary-compatible ASIO 2.x COM ABI definitions for Windows.
// Struct packing matches Steinberg's Windows x86/x64 ASIO2 ABI (4-byte packing).
#pragma pack(push, 4)

using ASIOBool = long;
constexpr ASIOBool ASIOFalse = 0;
constexpr ASIOBool ASIOTrue  = 1;

using ASIOSampleRate = double;

struct ASIOSamples {
    uint32_t hi;
    uint32_t lo;
};

struct ASIOTimeStamp {
    uint32_t hi;
    uint32_t lo;
};

using ASIOSampleType = long;
enum : ASIOSampleType {
    ASIOSTInt16MSB   = 0,
    ASIOSTInt24MSB   = 1,
    ASIOSTInt32MSB   = 2,
    ASIOSTFloat32MSB = 3,
    ASIOSTFloat64MSB = 4,

    ASIOSTInt32MSB16 = 8,
    ASIOSTInt32MSB18 = 9,
    ASIOSTInt32MSB20 = 10,
    ASIOSTInt32MSB24 = 11,

    ASIOSTInt16LSB   = 16,
    ASIOSTInt24LSB   = 17,
    ASIOSTInt32LSB   = 18,
    ASIOSTFloat32LSB = 19,
    ASIOSTFloat64LSB = 20,

    ASIOSTInt32LSB16 = 24,
    ASIOSTInt32LSB18 = 25,
    ASIOSTInt32LSB20 = 26,
    ASIOSTInt32LSB24 = 27
};

using ASIOError = long;
enum : ASIOError {
    ASE_OK               = 0,
    ASE_SUCCESS          = 0x3f4847a0,
    ASE_NotPresent       = -1000,
    ASE_HWMalfunction    = -999,
    ASE_InvalidParameter = -998,
    ASE_InvalidMode      = -997,
    ASE_SPNotAdvancing   = -996,
    ASE_NoClock          = -995,
    ASE_NoMemory         = -994
};

// Host message selectors
enum : long {
    kAsioSelectorSupported    = 1,
    kAsioEngineVersion        = 2,
    kAsioResetRequest         = 3,
    kAsioBufferSizeChange     = 4,
    kAsioResyncRequest        = 5,
    kAsioLatenciesChanged     = 6,
    kAsioSupportsTimeInfo     = 7,
    kAsioSupportsTimeCode     = 8,
    kAsioMMCCommand           = 9,
    kAsioSupportsInputMonitor = 10,
    kAsioSupportsInputGain    = 11,
    kAsioSupportsInputMeter   = 12,
    kAsioSupportsOutputGain   = 13,
    kAsioSupportsOutputMeter  = 14,
    kAsioOverload             = 15
};

// Future selectors
enum : long {
    kAsioEnableTimeCodeRead      = 1,
    kAsioDisableTimeCodeRead     = 2,
    kAsioSetInputMonitor         = 3,
    kAsioTransport               = 4,
    kAsioSetInputGain            = 5,
    kAsioGetInputMeter           = 6,
    kAsioSetOutputGain           = 7,
    kAsioGetOutputMeter          = 8,
    kAsioCanInputMonitor         = 9,
    kAsioCanTimeInfo             = 10,
    kAsioCanTimeCode             = 11,
    kAsioCanTransport            = 12,
    kAsioCanInputGain            = 13,
    kAsioCanInputMeter           = 14,
    kAsioCanOutputGain           = 15,
    kAsioCanOutputMeter          = 16,
    kAsioOptionalOne             = 17,
    kAsioSetIoFormat             = 0x23111961,
    kAsioGetIoFormat             = 0x23111983,
    kAsioCanDoIoFormat           = 0x23112004,
    kAsioCanReportOverload       = 0x24042012,
    kAsioGetInternalBufferSamples = 0x25042012
};

struct ASIOTimeCode {
    double        speed;
    ASIOSamples   timeCodeSamples;
    unsigned long flags;
    char          future[64];
};

struct ASIOTimeInfo {
    double         speed;
    ASIOTimeStamp  systemTime;
    ASIOSamples    samplePosition;
    ASIOSampleRate sampleRate;
    unsigned long  flags;
    char           reserved[12];
};

enum : unsigned long {
    kSystemTimeValid     = 1,
    kSamplePositionValid = 1 << 1,
    kSampleRateValid     = 1 << 2,
    kSpeedValid          = 1 << 3,
    kSampleRateChanged   = 1 << 4,
    kClockSourceChanged  = 1 << 5
};

struct ASIOTime {
    long         reserved[4];
    ASIOTimeInfo timeInfo;
    ASIOTimeCode timeCode;
};

struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
};

struct ASIOClockSource {
    long index;
    long associatedChannel;
    long associatedGroup;
    ASIOBool isCurrentSource;
    char name[32];
};

struct ASIOChannelInfo {
    long           channel;
    ASIOBool       isInput;
    ASIOBool       isActive;
    long           channelGroup;
    ASIOSampleType type;
    char           name[32];
};

struct ASIOBufferInfo {
    ASIOBool isInput;
    long     channelNum;
    void*    buffers[2];
};

#pragma pack(pop)

// Compile-time binary ABI verification against Steinberg ASIO 2.3 Windows x64 layout
static_assert(sizeof(ASIOSamples) == 8, "ASIOSamples ABI mismatch");
static_assert(sizeof(ASIOTimeStamp) == 8, "ASIOTimeStamp ABI mismatch");
static_assert(sizeof(ASIOClockSource) == 48, "ASIOClockSource ABI mismatch");
static_assert(sizeof(ASIOChannelInfo) == 52, "ASIOChannelInfo ABI mismatch");
static_assert(sizeof(ASIOBufferInfo) == 24, "ASIOBufferInfo ABI mismatch (x64)");
static_assert(sizeof(ASIOTimeInfo) == 48, "ASIOTimeInfo ABI mismatch");
static_assert(sizeof(ASIOTimeCode) == 84, "ASIOTimeCode ABI mismatch");
static_assert(sizeof(ASIOTime) == 148, "ASIOTime ABI mismatch");

// IASIO COM interface (vtable order must remain exact)
interface IASIO : public IUnknown {
    virtual ASIOBool  init(void* sysHandle) = 0;
    virtual void      getDriverName(char* name) = 0;
    virtual long      getDriverVersion() = 0;
    virtual void      getErrorMessage(char* string) = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
    virtual ASIOError setClockSource(long reference) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(long selector, void* opt) = 0;
    virtual ASIOError outputReady() = 0;
};
