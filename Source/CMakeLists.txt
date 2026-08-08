set(BaseTargetName dexed-cli)

juce_add_console_app("${BaseTargetName}"
        VERSION "1.0.1"
        COMPANY_NAME "Digital Suburban"
        PRODUCT_NAME "dexed-cli"
        BUNDLE_ID "com.digitalsuburban.dexed-cli"
)

juce_generate_juce_header(${BaseTargetName})

include_directories(../libs/MTS-ESP/Client)

target_sources(${BaseTargetName} PRIVATE
        MainCli.cpp
        RenderJob.cpp
        EngineMkI.cpp
        EngineOpl.cpp
        PluginData.cpp
        PluginFx.cpp
        PluginParam.cpp
        PluginProcessor.cpp
        msfa/dx7note.cc
        msfa/env.cc
        msfa/exp2.cc
        msfa/fm_core.cc
        msfa/fm_op_kernel.cc
        msfa/freqlut.cc
        msfa/lfo.cc
        msfa/pitchenv.cc
        msfa/sin.cc
        msfa/tuning.cc
        msfa/porta.cpp

    # until MTS-ESP has is own cmake project
    ../libs/MTS-ESP/Client/libMTSClient.cpp
)

target_compile_definitions(${BaseTargetName} PUBLIC
    DEXED_HEADLESS=1
    JucePlugin_Name="Dexed"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STANDALONE_APPLICATION=1
    # No audio hardware is ever opened, so the device backends are dead weight.
    JUCE_JACK=0
    JUCE_ALSA=0
)

# juce_audio_processors pulls juce_gui_extra -> juce_gui_basics -> juce_graphics
# in through its module dependencies, so those get linked whether we want them
# or not. Nothing here ever constructs a Component, so no display is opened:
# JUCE dlopens libX11 lazily and we never reach that path.
target_link_libraries(${BaseTargetName}
PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    tuning-library
    DexedResources
PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
)
