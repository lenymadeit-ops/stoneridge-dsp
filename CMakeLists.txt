cmake_minimum_required(VERSION 3.24)
project(StoneridgeASIO VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(STONERIDGE_ENABLE_ASAN "Build with MSVC AddressSanitizer" OFF)
option(STONERIDGE_ENABLE_ANALYZE "Build our targets with MSVC /analyze /WX zero-warning static analysis" OFF)

# Force static MSVC runtime (/MT) for Release; ASan uses /MD for MSVC clang_rt.asan_dynamic
if(STONERIDGE_ENABLE_ASAN)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(MSVC)
    add_compile_options(
        /W4
        /permissive-
        /Zc:__cplusplus
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/Oi>
        $<$<CONFIG:Release>:/Ot>
        $<$<CONFIG:Release>:/arch:AVX2>
        $<$<CONFIG:Release>:/fp:fast>
    )
    if(STONERIDGE_ENABLE_ASAN)
        add_compile_options(/fsanitize=address /Zi)
    endif()
    add_compile_definitions(
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
endif()

function(stoneridge_apply_strict_flags target_name)
    if(MSVC AND STONERIDGE_ENABLE_ANALYZE)
        target_compile_options(${target_name} PRIVATE /analyze /WX /analyze:external- /external:anglebrackets /external:W0)
    endif()
endfunction()

include(cmake/Dependencies.cmake)

# ==============================================================================
# 1. Core DSP & Lock-Free Engine Library (shared by ASIO DLL & Unit Tests)
# ==============================================================================
add_library(StoneridgeCore STATIC
    src/dsp/ControlRoomMonitor.h
    src/dsp/DacConditioner.h
    src/dsp/Em91cuCalibration.h
    src/dsp/InputProcessor.cpp
    src/dsp/InputProcessor.h
    src/vocal/La2aCalibration.h
    src/vocal/LoudnessMatcher.h
    src/vocal/MicLocker.h
    src/vocal/MicLockerCurves.h
    src/vocal/Oversampler.h
    src/vocal/PreampAndConsole.h
    src/vocal/VocalDynamics.h
    src/engine/AdaptiveDriftResampler.h
    src/engine/AsioHostBridge.h
    src/engine/AudioFormatConvert.h
    src/engine/IAudioStreamBackend.h
    src/engine/LockFreeRingBuffer.h
    src/engine/MackieHidMonitor.cpp
    src/engine/MackieHidMonitor.h
    src/engine/OsRealtimeTuning.h
    src/engine/WasapiDeviceManager.cpp
    src/engine/WasapiDeviceManager.h
    src/engine/WasapiExclusiveStream.cpp
    src/engine/WasapiExclusiveStream.h
    src/engine/WdmKsHardwareProbe.cpp
    src/engine/WdmKsHardwareProbe.h
    src/engine/WdmKsStream.cpp
    src/engine/WdmKsStream.h
    src/ipc/SharedState.h
)

target_include_directories(StoneridgeCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(StoneridgeCore PUBLIC
    WIL::WIL
    avrt
    ole32
    mmdevapi
    winmm
    setupapi
    hid
)
stoneridge_apply_strict_flags(StoneridgeCore)

# ==============================================================================
# 2. ASIO COM Driver DLL (StoneridgeASIO_x64.dll)
# ==============================================================================
add_library(StoneridgeASIO SHARED
    src/asio/asio_abi.h
    src/asio/StoneridgeASIO.cpp
    src/asio/StoneridgeASIO.h
    src/asio/StoneridgeASIO.def
    src/asio/DllMain.cpp
)

set_target_properties(StoneridgeASIO PROPERTIES
    OUTPUT_NAME "StoneridgeASIO_x64"
)

target_link_libraries(StoneridgeASIO PRIVATE
    StoneridgeCore
    nlohmann_json::nlohmann_json
    ole32
    advapi32
    shlwapi
    shell32
)
stoneridge_apply_strict_flags(StoneridgeASIO)

# ==============================================================================
# 3. Out-of-Process Apple-Style WebView2 Control Panel (StoneridgePanel.exe)
# ==============================================================================
add_executable(StoneridgePanel WIN32
    src/ui/PanelMain.cpp
    src/ui/PanelResources.rc
)

target_include_directories(StoneridgePanel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(StoneridgePanel PRIVATE
    WIL::WIL
    WebView2::Static
    nlohmann_json::nlohmann_json
    dwmapi
    shlwapi
    shell32
    ole32
    user32
    gdi32
)

# Copy web assets next to the executable after build
add_custom_command(TARGET StoneridgePanel POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/web"
        "$<TARGET_FILE_DIR:StoneridgePanel>/web"
    COMMENT "Copying Apple-style WebView2 UI assets..."
)

# ==============================================================================
# 4. Automated Unit Tests & DSP Benchmarks
# ==============================================================================
enable_testing()
add_executable(StoneridgeTests
    tests/TestRingBuffer.cpp
    tests/TestInputDsp.cpp
    tests/TestFormatIntegrity.cpp
    tests/TestAudioContinuity.cpp
    tests/TestSpectralAndFilters.cpp
    tests/TestChaosIpc.cpp
    tests/BenchmarkDsp.cpp
    tests/DiagHardware.cpp
    tests/DiagWdmKs.cpp
    tests/StressHardware.cpp
)

target_link_libraries(StoneridgeTests PRIVATE
    StoneridgeCore
    Catch2::Catch2WithMain
)

set_target_properties(StoneridgeTests PROPERTIES
    LINK_OPTIONS "/ENTRY:wmainCRTStartup"
)

# Standalone COM ASIO2 Conformance Host (loads actual registered StoneridgeASIO_x64.dll via CoCreateInstance)
add_executable(AsioConformanceHost
    tests/AsioConformanceHost.cpp
)
target_include_directories(AsioConformanceHost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AsioConformanceHost PRIVATE ole32 user32)

# Head-to-Head COM ASIO Shootout: Stoneridge ASIO vs. ASIO4ALL v2
add_executable(ShootoutBench
    tests/ShootoutBench.cpp
)
target_include_directories(ShootoutBench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ShootoutBench PRIVATE ole32 user32)

include(CTest)
include(Catch)
catch_discover_tests(StoneridgeTests)
