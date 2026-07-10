cmake_minimum_required(VERSION 4.2)

project(
    CapturePanelWindows
    VERSION 0.1.0
    DESCRIPTION "Capture Panel core and command-line application for Windows"
    LANGUAGES CXX
)

option(CAPTURE_PANEL_BUILD_TESTS "Build Capture Panel tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(capture_panel_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    endif()
endfunction()

add_library(capture_panel_core STATIC
    src/core/src/audio.cpp
    src/core/src/alignment.cpp
    src/core/src/capture.cpp
    src/core/src/channels.cpp
    src/core/src/verification.cpp
    src/core/src/wav.cpp
)
target_include_directories(capture_panel_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/include>
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

add_library(capture_panel_cli_lib STATIC
    src/cli/cli.cpp
)
target_include_directories(capture_panel_cli_lib PUBLIC src/cli)
target_link_libraries(capture_panel_cli_lib PUBLIC capture_panel_core capture_panel_fake)
target_compile_definitions(
    capture_panel_cli_lib
    PRIVATE CAPTURE_PANEL_VERSION="${PROJECT_VERSION}"
)
capture_panel_enable_warnings(capture_panel_cli_lib)

add_executable(capture-panel src/cli/main.cpp)
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
        tests/audio_tests.cpp
        tests/capture_tests.cpp
        tests/channel_tests.cpp
        tests/cli_tests.cpp
        tests/fake_backend_tests.cpp
        tests/verification_tests.cpp
        tests/wav_tests.cpp
    )
    target_include_directories(capture_panel_tests PRIVATE tests)
    target_link_libraries(capture_panel_tests
        PRIVATE
            capture_panel_core
            capture_panel_fake
            capture_panel_cli_lib
    )
    capture_panel_enable_warnings(capture_panel_tests)
    add_test(NAME capture_panel_tests COMMAND capture_panel_tests)
endif()
