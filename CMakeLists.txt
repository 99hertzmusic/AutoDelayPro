cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")

project(AutoDelayPro VERSION 1.0.0)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(AUTODELAYPRO_COPY_PLUGIN_AFTER_BUILD "Copy built plugins into local plug-in folders" ON)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.13
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_plugin(AutoDelayPro
    COMPANY_NAME "99Hertz"
    BUNDLE_ID "com.99hertz.autodelaypro"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${AUTODELAYPRO_COPY_PLUGIN_AFTER_BUILD}
    PLUGIN_MANUFACTURER_CODE Hz99
    PLUGIN_CODE ADlP
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "AutoDelay Pro"
)

juce_add_binary_data(AutoDelayProData SOURCES
    Resources/Fonts/BarlowCondensed-Bold.ttf
    Resources/Fonts/BarlowCondensed-SemiBold.ttf
    Resources/Fonts/BarlowCondensed-Medium.ttf
    Resources/Fonts/ShareTechMono-Regular.ttf)
set_target_properties(AutoDelayProData PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(AUTODELAYPRO_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    
    Source/Licence/Licence.cpp
    Source/Licence/tweetnacl.c)

target_sources(AutoDelayPro PRIVATE ${AUTODELAYPRO_SOURCES})

target_compile_definitions(AutoDelayPro PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_compile_features(AutoDelayPro PUBLIC cxx_std_17)

target_link_libraries(AutoDelayPro
    PRIVATE AutoDelayProData juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Offline test: grid timing, darkening, feedback bound, ducking, throws, clicks, state, Lite gating, keys, demos, UI (not shipped)
option(AUTODELAYPRO_BUILD_TEST "Build the offline test app" OFF)
if(AUTODELAYPRO_BUILD_TEST)
    juce_add_console_app(AutoDelayProTest PRODUCT_NAME "AutoDelayProTest")
    target_sources(AutoDelayProTest PRIVATE Test/Main.cpp ${AUTODELAYPRO_SOURCES})
    target_compile_definitions(AutoDelayProTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1
        AUTODELAYPRO_TEST=1 JucePlugin_VersionString="1.0.0")
    target_compile_features(AutoDelayProTest PRIVATE cxx_std_17)
    target_link_libraries(AutoDelayProTest PRIVATE AutoDelayProData juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
endif()
