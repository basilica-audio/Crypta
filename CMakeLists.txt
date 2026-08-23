cmake_minimum_required(VERSION 3.24)

# Must be set before the first project() call to take effect.
# CI overrides CMAKE_OSX_ARCHITECTURES explicitly for release/universal builds;
# local builds are left at the native architecture.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target" FORCE)


# JUCE's build tooling (juceaide) and some CMake find-modules it triggers
# (e.g. FindThreads) expect the C toolchain to be available; declaring it
# here at the top level avoids it being enabled implicitly deeper inside
# JUCE's CMake helpers, which has been observed to break Ninja's generate
# step (missing CMAKE_C_COMPILE_OBJECT rule variable).
project(Crypta VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/CPM.cmake)

# CPMAddPackage automatically respects the CPM_SOURCE_CACHE environment
# variable if it is set, reusing a shared package cache across builds/checkouts.
if(DEFINED ENV{CPM_SOURCE_CACHE})
    message(STATUS "Crypta: using CPM_SOURCE_CACHE at $ENV{CPM_SOURCE_CACHE}")
endif()

CPMAddPackage(
    NAME JUCE
    GITHUB_REPOSITORY juce-framework/JUCE
    GIT_TAG 8.0.14
    GIT_SHALLOW TRUE
)

# ==============================================================================
# Plugin target
# ==============================================================================

juce_add_plugin(Crypta
    COMPANY_NAME "Yves Vogl"
    BUNDLE_ID com.yvesvogl.crypta
    PLUGIN_MANUFACTURER_CODE Yvsv
    PLUGIN_CODE Cryp
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "Crypta"
    COPY_PLUGIN_AFTER_BUILD TRUE
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    ICON_BIG "docs/assets/icon.png"
)

# ==============================================================================
# Embedded assets: M2 preset system factory presets (presets/factory/*.json)
# and the M2 i18n frame's German translation (resources/i18n/de.txt). See
# src/presets/PresetManager.h and src/presets/Localisation.h - both consume
# these purely via BinaryData:: symbols passed in from PluginProcessor.cpp/
# PluginEditor.cpp, never by including BinaryData.h themselves, so they stay
# portable to sibling plugins (see docs/preset-system-notes.md in
# basilica-audio/nave, the M2 pilot this suite-wide pattern was replicated
# from).
# ==============================================================================

juce_add_binary_data(CryptaBinaryData SOURCES
    presets/factory/default.json
    presets/factory/glueAndGrind.json
    presets/factory/subLock.json
    presets/factory/throat.json
    presets/factory/fuzzWall.json
    presets/factory/cutThrough.json
    presets/factory/definitionOnly.json
    presets/factory/cleanLowLoudTop.json
    presets/factory/cabColoredGrind.json
    presets/factory/circuitFoundation.json
    presets/factory/circuitGrind.json
    presets/factory/circuitKnife.json
    resources/i18n/de.txt
    # Suite typography (M3/M6 vector editor, issues #45/#25): EB Garamond,
    # OFL licensed (resources/fonts/OFL-EBGaramond.txt) - consumed by
    # src/gui/BasilicaLookAndFeel.cpp as the single source of every glyph
    # the fully JUCE-drawn editor renders.
    resources/fonts/EBGaramond-Regular.ttf
    resources/fonts/EBGaramond-SemiBold.ttf
    # Factory cabinet impulse responses (issues #21/#81). Every one of these is
    # GENERATED, not recorded: tools/ir-synth/cabsynth.py computes them from a
    # documented analytical cabinet model, so there is no third-party recording
    # inside the shipped binary and therefore no provenance to trace or dispute.
    # Licensing, per-file model parameters, checksums and the measured response
    # of each are in resources/irs/LICENSES.md; the CC0 dedication these are
    # published under is committed at resources/irs/CC0-1.0.txt. Registered in
    # CryptaAudioProcessor::getFactoryIRAssetTable(), guarded by the licence bar
    # in src/dsp/FactoryIRs.cpp and re-measured by tests/FactoryIRTests.cpp.
    resources/irs/modelled_8x10_cone.wav
    resources/irs/modelled_8x10_edge.wav
    resources/irs/modelled_1x15_vintage.wav
    resources/irs/modelled_4x10_horn.wav
)

# ==============================================================================
# SharedCode: an INTERFACE target carrying the plugin's sources, definitions,
# and library links so that both the plugin targets (AU/VST3/Standalone) and
# the Tests executable compile the exact same code (Pamplejuce pattern, see
# https://github.com/sudara/pamplejuce), avoiding ODR violations between them.
# ==============================================================================

add_library(SharedCode INTERFACE)

target_compile_features(SharedCode INTERFACE cxx_std_20)

file(GLOB_RECURSE CryptaSources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

target_sources(SharedCode INTERFACE ${CryptaSources})

target_include_directories(SharedCode INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(SharedCode INTERFACE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    # AGPLv3 usage permits disabling the splash screen.
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(SharedCode INTERFACE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
    CryptaBinaryData
)

target_link_libraries(Crypta PRIVATE SharedCode)

# ==============================================================================
# Tests
# ==============================================================================

enable_testing()
set(CTEST_OUTPUT_ON_FAILURE ON)

CPMAddPackage(
    NAME Catch2
    GITHUB_REPOSITORY catchorg/Catch2
    GIT_TAG v3.15.2
)

file(GLOB_RECURSE CryptaTestSources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h")

add_executable(Tests ${CryptaTestSources})

target_compile_features(Tests PRIVATE cxx_std_20)

target_link_libraries(Tests PRIVATE SharedCode Catch2::Catch2)

# Copy over the plugin target's compile definitions (JucePlugin_* macros etc.)
# so plugin code compiles identically when built as part of the Tests binary.
target_compile_definitions(Tests PRIVATE
    $<TARGET_PROPERTY:Crypta,COMPILE_DEFINITIONS>
    JUCE_MODAL_LOOPS_PERMITTED=1
    # tests/QaGateManifestTests.cpp reads scripts/qa-gates.tsv - the QA gate's
    # declaration of which Catch2 tags make up docs/qa-checklist.md Part 1 -
    # and asserts every tag in it is still carried by a registered test case.
    # A repository file has to be located from the test binary to do that, and
    # the binary's own location moves with the generator and configuration.
    CRYPTA_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
)

set_target_properties(Tests PROPERTIES XCODE_GENERATE_SCHEME ON)

include(${Catch2_SOURCE_DIR}/extras/Catch.cmake)

if(CMAKE_GENERATOR STREQUAL "Xcode")
    set(CryptaDiscoveryMode PRE_TEST)
else()
    set(CryptaDiscoveryMode POST_BUILD)
endif()

catch_discover_tests(Tests DISCOVERY_MODE ${CryptaDiscoveryMode})
