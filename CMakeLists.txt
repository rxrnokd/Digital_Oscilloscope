cmake_minimum_required(VERSION 3.21)

project(SignalLabPro
    VERSION 1.0.0
    DESCRIPTION "SIGNAL LAB PRO - digital oscilloscope & signal analyzer"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SLP_BUILD_APP   "Build the Qt desktop application" ON)
option(SLP_BUILD_TESTS "Build the automated tests" ON)
option(SLP_USE_ACCELERATE "Use Apple Accelerate (vDSP) for FFT / vector DSP on macOS" ON)

find_package(Threads REQUIRED)

# --------------------------------------------------------------------------------
# Core signal chain: pure C++20, no Qt (generator, acquisition, trigger, DSP/FFT,
# measurements, file I/O). Unit tested without any GUI.
# --------------------------------------------------------------------------------
add_library(slp_core STATIC
    src/core/ScopeSettings.h   src/core/ScopeSettings.cpp
    src/core/RingBuffer.h
    src/core/SignalGenerator.h src/core/SignalGenerator.cpp
    src/core/Measurements.h    src/core/Measurements.cpp
    src/core/Trigger.h         src/core/Trigger.cpp
    src/core/Dsp.h             src/core/Dsp.cpp
    src/core/FileIO.h          src/core/FileIO.cpp
    src/core/Acquisition.h     src/core/Acquisition.cpp
)
target_include_directories(slp_core PUBLIC src/core)
target_link_libraries(slp_core PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(slp_core PRIVATE /W4)
else()
    target_compile_options(slp_core PRIVATE -Wall -Wextra -Wno-sign-compare)
endif()
if(APPLE AND SLP_USE_ACCELERATE)
    find_library(ACCELERATE_FRAMEWORK Accelerate REQUIRED)
    target_link_libraries(slp_core PUBLIC ${ACCELERATE_FRAMEWORK})
    target_compile_definitions(slp_core PUBLIC SLP_HAVE_ACCELERATE=1)
    message(STATUS "SIGNAL LAB PRO: FFT backend = Apple Accelerate vDSP")
else()
    message(STATUS "SIGNAL LAB PRO: FFT backend = portable radix-2")
endif()

# --------------------------------------------------------------------------------
# Qt based parts
# --------------------------------------------------------------------------------
if(SLP_BUILD_APP OR SLP_BUILD_TESTS)
    find_package(Qt6 6.2 REQUIRED COMPONENTS Core)
endif()

if(SLP_BUILD_APP OR SLP_BUILD_TESTS)
    # Setup (JSON) persistence - shared by the app and the restore test
    add_library(slp_settings STATIC src/app/SettingsJson.h src/app/SettingsJson.cpp)
    target_include_directories(slp_settings PUBLIC src/app)
    target_link_libraries(slp_settings PUBLIC slp_core Qt6::Core)
endif()

if(SLP_BUILD_APP)
    find_package(Qt6 6.2 REQUIRED COMPONENTS Gui Widgets)
    find_package(Qt6 6.2 QUIET COMPONENTS Multimedia)
    set(CMAKE_AUTOMOC ON)

    # Widgets (renderers, knobs, theme) - shared by the app and the GUI tests
    add_library(slp_gui STATIC
        src/app/Controls.h     src/app/Controls.cpp
        src/app/ScopeView.h    src/app/ScopeView.cpp
        src/app/SpectrumView.h src/app/SpectrumView.cpp)
    target_include_directories(slp_gui PUBLIC src/app)
    target_link_libraries(slp_gui PUBLIC slp_core Qt6::Widgets Qt6::Gui)

    set(APP_SOURCES
        src/app/main.cpp
        src/app/MainWindow.h   src/app/MainWindow.cpp
        src/app/AudioInput.h   src/app/AudioInput.cpp
    )
    qt_add_executable(SignalLabPro MACOSX_BUNDLE WIN32 ${APP_SOURCES})
    target_link_libraries(SignalLabPro PRIVATE slp_gui slp_settings)
    if(Qt6Multimedia_FOUND)
        target_link_libraries(SignalLabPro PRIVATE Qt6::Multimedia)
        target_compile_definitions(SignalLabPro PRIVATE SLP_HAVE_AUDIO=1)
        message(STATUS "SIGNAL LAB PRO: audio input via Qt Multimedia enabled")
    else()
        message(STATUS "SIGNAL LAB PRO: Qt Multimedia not found - audio input disabled")
    endif()
    if(NOT MSVC)
        target_compile_options(SignalLabPro PRIVATE -Wall -Wextra -Wno-sign-compare)
    endif()
    set_target_properties(SignalLabPro PROPERTIES
        OUTPUT_NAME "SignalLabPro"
        MACOSX_BUNDLE_BUNDLE_NAME "SIGNAL LAB PRO"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.signallabpro.app"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in")
endif()

# --------------------------------------------------------------------------------
# Tests (ctest)
# --------------------------------------------------------------------------------
if(SLP_BUILD_TESTS)
    enable_testing()
    add_executable(slp_tests tests/test_core.cpp tests/TestFramework.h)
    target_link_libraries(slp_tests PRIVATE slp_core)
    set(SLP_CORE_TESTS
        format_and_parse ring_buffer
        generator_sine_frequency_amplitude generator_square_duty_levels generator_pulse_edge_time
        generator_triangle_sawtooth_rms generator_dc_and_noise generator_phase_and_continuity
        generator_custom_composite measurement_na_rules
        fft_matches_naive_dft fft_peak_accuracy_and_normalisation fft_harmonics_thd
        trigger_edge_detection_interpolated trigger_hysteresis_and_holdoff
        engine_triggered_frame_alignment engine_trigger_modes engine_delay_offset
        engine_sample_rate_change engine_decimation_and_roll engine_multichannel
        engine_ac_coupling engine_segmented_high_rate engine_autoscale
        filter_frequency_response filter_time_domain math_arithmetic_derivative_integral
        engine_math_and_spectrum
        file_csv_roundtrip file_wav_roundtrip engine_file_source engine_push_source)
    foreach(t IN LISTS SLP_CORE_TESTS)
        add_test(NAME ${t} COMMAND slp_tests ${t})
    endforeach()

    add_executable(slp_settings_tests tests/test_settings.cpp tests/TestFramework.h)
    target_link_libraries(slp_settings_tests PRIVATE slp_settings)
    foreach(t settings_json_roundtrip settings_defaults_and_robustness settings_file_save_load)
        add_test(NAME ${t} COMMAND slp_settings_tests ${t})
    endforeach()

    if(SLP_BUILD_APP)
        find_package(Qt6 6.2 QUIET COMPONENTS Test)
        if(Qt6Test_FOUND)
            add_executable(slp_gui_tests tests/test_gui.cpp)
            target_link_libraries(slp_gui_tests PRIVATE slp_gui Qt6::Test)
            add_test(NAME gui_interaction COMMAND slp_gui_tests)
            set_tests_properties(gui_interaction PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        endif()
        # End-to-end restart test: launch the real application, load a setup, quit
        # (session saved), relaunch and verify that the setup was restored.
        set(SLP_APP_EXE "$<TARGET_FILE:SignalLabPro>")
        add_test(NAME app_restart_restores_settings
            COMMAND ${CMAKE_COMMAND}
                -DAPP=${SLP_APP_EXE}
                -DSETUP=${CMAKE_CURRENT_SOURCE_DIR}/examples/setups/am_modulation.json
                -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/restart_test
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/restart_test.cmake)
        set_tests_properties(app_restart_restores_settings PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
    endif()
endif()
