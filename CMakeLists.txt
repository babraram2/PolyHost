cmake_minimum_required(VERSION 3.22)
project(PolyHost VERSION 0.1.0)

# Put JUCE next to this file:
#   git submodule add https://github.com/juce-framework/JUCE.git JUCE
add_subdirectory(JUCE)

juce_add_plugin(PolyHost
    COMPANY_NAME            "YourName"
    IS_SYNTH                TRUE
    NEEDS_MIDI_INPUT        TRUE
    NEEDS_MIDI_OUTPUT       FALSE
    PLUGIN_MANUFACTURER_CODE Poly
    PLUGIN_CODE             PHst
    FORMATS                 VST3 Standalone
    PRODUCT_NAME            "PolyHost")

juce_generate_juce_header(PolyHost)

target_sources(PolyHost PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(PolyHost PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_PLUGINHOST_VST3=1          # lets us LOAD VST3 plugins inside our plugin
    JUCE_PLUGINHOST_AU=$<BOOL:${APPLE}>)

target_link_libraries(PolyHost
    PRIVATE
        juce::juce_audio_utils      # includes processors + PluginListComponent
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
