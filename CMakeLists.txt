cmake_minimum_required(VERSION 4.2)

project(
    CapturePanelWindows
    VERSION 0.1.0
    DESCRIPTION "Capture Panel core and command-line application for Windows"
    LANGUAGES CXX
)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Capture Panel's ASIO host requires 64-bit Windows.")
endif()

enable_language(RC)

option(CAPTURE_PANEL_BUILD_TESTS "Build Capture Panel tests" ON)

set(CAPTURE_PANEL_WINDOWS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/windows/CapturePanel.ico")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    src/cli/capture-panel.rc.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/capture-panel.rc"
    @ONLY
)
set(CAPTURE_PANEL_CLI_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/capture-panel.rc")
set_source_files_properties(
    "${CAPTURE_PANEL_CLI_RESOURCE}"
    PROPERTIES OBJECT_DEPENDS "${CAPTURE_PANEL_WINDOWS_ICON}"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    # Release archives are self-contained and do not require a separately
    # installed Visual C++ Redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

function(capture_panel_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    endif()
endfunction()

add_library(capture_panel_windows_platform STATIC
    src/platform/windows/atomic_file_transaction.cpp
)
target_include_directories(capture_panel_windows_platform
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows>
)
target_compile_features(capture_panel_windows_platform PRIVATE cxx_std_20)
capture_panel_enable_warnings(capture_panel_windows_platform)

add_library(capture_panel_core STATIC
    src/core/src/audio.cpp
    src/core/src/alignment.cpp
    src/core/src/capture.cpp
    src/core/src/channels.cpp
    src/core/src/streaming.cpp
    src/core/src/verification.cpp
    src/core/src/wav.cpp
)
target_link_libraries(capture_panel_core PRIVATE capture_panel_windows_platform)
target_include_directories(capture_panel_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/include>
    PRIVATE
        src/platform/windows
)
target_compile_features(capture_panel_core PUBLIC cxx_std_20)
capture_panel_enable_warnings(capture_panel_core)

add_library(capture_panel_fake STATIC
    src/backends/fake/src/fake_backend.cpp
)
target_include_directories(capture_panel_fake
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/backends/fake/include>
)
target_link_libraries(capture_panel_fake PUBLIC capture_panel_core)
capture_panel_enable_warnings(capture_panel_fake)

add_library(capture_panel_asio STATIC
    src/backends/asio/src/asio_backend.cpp
    src/backends/asio/src/asio_capture.cpp
    src/backends/asio/src/asio_driver_registry.cpp
    src/backends/asio/src/asio_driver_session.cpp
    src/backends/asio/src/asio_sample_converter.cpp
    src/backends/asio/src/asio_stream_workers.cpp
    src/backends/asio/src/asio_temporary_float_file.cpp
)
target_include_directories(capture_panel_asio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/backends/asio/include>
    PRIVATE
        src/backends/asio/src
        third_party/asio/common
)
target_link_libraries(capture_panel_asio
    PUBLIC capture_panel_core
    PRIVATE Advapi32 Ole32 User32
)
target_compile_features(capture_panel_asio PUBLIC cxx_std_20)
capture_panel_enable_warnings(capture_panel_asio)

add_library(capture_panel_backends STATIC
    src/backends/router/src/backend_router.cpp
)
target_include_directories(capture_panel_backends
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/backends/router/include>
)
target_link_libraries(capture_panel_backends PUBLIC capture_panel_core)
capture_panel_enable_warnings(capture_panel_backends)

add_library(capture_panel_cli_lib STATIC
    src/cli/cli.cpp
)
target_include_directories(capture_panel_cli_lib PUBLIC src/cli)
target_link_libraries(capture_panel_cli_lib
    PUBLIC
        capture_panel_core
        capture_panel_fake
        capture_panel_asio
        capture_panel_backends
)
target_compile_definitions(
    capture_panel_cli_lib
    PRIVATE CAPTURE_PANEL_VERSION="${PROJECT_VERSION}"
)
capture_panel_enable_warnings(capture_panel_cli_lib)

add_executable(capture-panel
    src/cli/main.cpp
    src/cli/app.manifest
    "${CAPTURE_PANEL_CLI_RESOURCE}"
)
target_link_libraries(capture-panel PRIVATE capture_panel_cli_lib)
capture_panel_enable_warnings(capture-panel)

set_target_properties(capture-panel PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(CAPTURE_PANEL_BUILD_TESTS)
    include(CTest)
    enable_testing()

    add_executable(capture_panel_tests
        tests/test_main.cpp
        tests/alignment_tests.cpp
        tests/asio_callback_lifetime_tests.cpp
        tests/asio_buffer_timeline_tests.cpp
        tests/asio_driver_registry_tests.cpp
        tests/asio_sample_converter_tests.cpp
        tests/asio_spsc_ring_tests.cpp
        tests/asio_stream_workers_tests.cpp
        tests/asio_temporary_float_file_tests.cpp
        tests/audio_tests.cpp
        tests/backend_router_tests.cpp
        tests/capture_tests.cpp
        tests/channel_tests.cpp
        tests/cli_tests.cpp
        tests/fake_backend_tests.cpp
        tests/streaming_tests.cpp
        tests/verification_tests.cpp
        tests/wav_tests.cpp
    )
    target_include_directories(capture_panel_tests
        PRIVATE
            tests
            src/backends/asio/src
            third_party/asio/common
    )
    target_link_libraries(capture_panel_tests
        PRIVATE
            capture_panel_core
            capture_panel_fake
            capture_panel_asio
            capture_panel_backends
            capture_panel_cli_lib
    )
    capture_panel_enable_warnings(capture_panel_tests)
    add_test(NAME capture_panel_tests COMMAND capture_panel_tests)
endif()
