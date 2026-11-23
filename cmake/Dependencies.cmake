include(FetchContent)

# Microsoft WIL (Windows Implementation Libraries) - RAII COM & Handle wrappers
FetchContent_Declare(
    wil
    GIT_REPOSITORY https://github.com/microsoft/wil.git
    GIT_TAG        v1.0.240803.1
    GIT_SHALLOW    TRUE
)

# nlohmann/json - Configuration & presets (UI process only, never audio thread)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

# Catch2 - Unit tests & DSP benchmarks
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
)

# Microsoft WebView2 SDK (NuGet package is a standard zip archive)
FetchContent_Declare(
    webview2
    URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2903.40
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(WIL_BUILD_PACKAGING OFF CACHE BOOL "" FORCE)
set(WIL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE INTERNAL "")

FetchContent_MakeAvailable(wil nlohmann_json Catch2 webview2)

# Create an imported interface target for WebView2 static loader
if(NOT TARGET WebView2::Static)
    add_library(WebView2::Static INTERFACE IMPORTED)
    set_target_properties(WebView2::Static PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${webview2_SOURCE_DIR}/build/native/include"
        INTERFACE_LINK_LIBRARIES "${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
    )
endif()
