# VST3 install path — user-level (no admin required), scanned by Reaper, Ableton, FL Studio
if(WIN32)
    set(KHENSU_VST3_COPY_DIR "$ENV{LOCALAPPDATA}/Programs/Common/VST3")
elseif(APPLE)
    set(KHENSU_VST3_COPY_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/VST3")
else()
    set(KHENSU_VST3_COPY_DIR "$ENV{HOME}/.vst3")
endif()

# Optional SDKs (CE-DAW-1 / CE-DAW-2). Neither ever fails the configure: an
# offline machine just skips the ARA target and the .clap, with a message.
include(${CMAKE_SOURCE_DIR}/cmake/FetchARASDK.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/FetchClapJuceExtensions.cmake)

# AU on macOS builds (CE-DAW-2). Unverified until auval runs on a Mac.
set(KHENSU_COPILOT_FORMATS VST3)
if(APPLE)
    list(APPEND KHENSU_COPILOT_FORMATS AU)
endif()

# Sources shared by every Copilot target. The ARA document controller is
# guarded by JucePlugin_Enable_ARA, so it compiles to nothing in KhensuCopilot.
set(KHENSU_COPILOT_SOURCES
    Source/CopilotAnalysisController.cpp
    Source/CopilotCorrectiveDsp.cpp
    Source/CopilotOutputStage.cpp
    Source/CopilotRealtimeTelemetry.cpp
    Source/CopilotServiceJobs.cpp
    Source/CopilotTransportController.cpp
    Source/HostedPluginChainManager.cpp
    Source/HostedPluginEditorWindow.cpp
    Source/ModuleLifetime.cpp
    Source/PluginWorkerPool.cpp
    Source/PluginCrashHandler.cpp  # CE-ENG-4 local crash records
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/ui/MeterSection.cpp
    Source/ui/HookIndicatorPanel.cpp
    Source/ui/SuggestionFeedPanel.cpp
    Source/ui/SystemBrainMiniPanel.cpp
    Source/ui/MixerChainMiniPanel.cpp
    Source/ui/ChatConsoleMiniPanel.cpp
    Source/ui/PluginLookAndFeel.cpp
    Source/ui/DspControlGridPanel.cpp
    Source/ui/EqCurveMiniPanel.cpp
    Source/ui/HostedChainMiniPanel.cpp
    Source/ui/MixHubPanel.cpp
    Source/ui/IssueTimelinePanel.cpp
    Source/mixhub/MixHub.cpp
    Source/mixhub/MixHubAnalysis.cpp
    Source/timeline/IssueTimeline.cpp
    Source/live/LiveAnalysisFifo.cpp
    Source/live/WindowIssueScanner.cpp
    Source/live/CopilotLiveAnalyzer.cpp
    Source/automation/FixAutomation.cpp
    Source/ara/KhensuAraDocumentController.cpp
)

khensu_conversion_warnings(${KHENSU_COPILOT_SOURCES})

# Everything a Copilot target needs besides juce_add_plugin itself: sources,
# links, definitions, flags and the in-bundle resources.
function(khensu_configure_copilot_target target)
    juce_generate_juce_header(${target})
    target_sources(${target} PRIVATE ${KHENSU_COPILOT_SOURCES})
    target_link_libraries(${target} PRIVATE
        KhensuSharedCore
        juce::juce_audio_processors
        juce::juce_audio_basics
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_basics
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
    )
    target_compile_definitions(${target} PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_AU=1
    )
    khensu_apply_compiler_flags(${target} COMPILES_JUCE)
    foreach(_khensu_fmt VST3 AU AUv3 Standalone CLAP LV2 AAX Unity)
        if(TARGET ${target}_${_khensu_fmt})
            khensu_harden_link(${target}_${_khensu_fmt})
        endif()
    endforeach()

    # Ship the System Brain feature encyclopedia INSIDE the .vst3 bundle
    # (Contents/Resources), so the Hooks/Brain tab finds it in an installed
    # plugin (PLG-07). SystemBrainMiniPanel resolves it relative to the plugin
    # module, not the host EXE.
    set(_resources
        "$<GENEX_EVAL:$<TARGET_PROPERTY:${target}_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>/Contents/Resources")
    add_custom_command(TARGET ${target}_VST3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_resources}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/resources/system_brain/feature_encyclopedia.json"
            "${_resources}/feature_encyclopedia.json"
        VERBATIM)
endfunction()

juce_add_plugin(KhensuCopilot
    PRODUCT_NAME        "Khensu Copilot"
    VERSION             "1.0.2"
    BUNDLE_ID           "com.khensu.mixing.copilot"
    COMPANY_NAME        "Khensu"
    PLUGIN_MANUFACTURER_CODE KhMx
    PLUGIN_CODE         KCop
    FORMATS             ${KHENSU_COPILOT_FORMATS}
    IS_SYNTH            FALSE
    NEEDS_MIDI_INPUT    FALSE
    NEEDS_MIDI_OUTPUT   FALSE
    IS_MIDI_EFFECT      FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE   # Enable once build confirmed working; copy dir above
    VST3_COPY_DIR       "${KHENSU_VST3_COPY_DIR}"
)
khensu_configure_copilot_target(KhensuCopilot)

# ---- CLAP (CE-DAW-2) ------------------------------------------------------------
if(KHENSU_CLAP_AVAILABLE)
    clap_juce_extensions_plugin(TARGET KhensuCopilot
        CLAP_ID "com.khensu.copilot"
        CLAP_FEATURES audio-effect analyzer)
    message(STATUS "CLAP: KhensuCopilot_CLAP -> Khensu Copilot.clap")
endif()

# ---- ARA2 variant (CE-DAW-1) ----------------------------------------------------
# A separate target, so KhensuCopilot (and its pluginval gate) never changes:
# a distinct plugin code and bundle id, the same sources, IS_ARA_EFFECT on.
if(KHENSU_ARA_AVAILABLE)
    juce_add_plugin(KhensuCopilotARA
        PRODUCT_NAME        "Khensu Copilot ARA"
        VERSION             "1.0.2"
        BUNDLE_ID           "com.khensu.mixing.copilot.ara"
        COMPANY_NAME        "Khensu"
        PLUGIN_MANUFACTURER_CODE KhMx
        PLUGIN_CODE         KCoA
        FORMATS             ${KHENSU_COPILOT_FORMATS}
        IS_SYNTH            FALSE
        NEEDS_MIDI_INPUT    FALSE
        NEEDS_MIDI_OUTPUT   FALSE
        IS_MIDI_EFFECT      FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        IS_ARA_EFFECT       TRUE
        ARA_TRANSFORMATION_FLAGS kARAPlaybackTransformationNoChanges
        COPY_PLUGIN_AFTER_BUILD FALSE
        VST3_COPY_DIR       "${KHENSU_VST3_COPY_DIR}"
    )
    khensu_configure_copilot_target(KhensuCopilotARA)

    # JUCE compiles the SDK's ARADebug.c in its own translation unit, which
    # #defines WIN32_LEAN_AND_MEAN unconditionally; the project defines it
    # globally (top-level CMakeLists), so MSVC reports C4005 under /WX. Silence
    # that one warning for that one third-party file only.
    if(MSVC)
        set_source_files_properties(
            "${juce_SOURCE_DIR}/modules/juce_audio_processors_headless/juce_audio_processors_headless_ara.cpp"
            TARGET_DIRECTORY KhensuCopilotARA
            PROPERTIES COMPILE_OPTIONS "/wd4005")
    endif()
endif()

# WF-8: the plugin grades against the session's genre like the desktop, so it
# carries the genre targets itself (the app's resources folder is not next to
# a .vst3 bundle).
juce_add_binary_data(KhensuCopilotGenreData
    NAMESPACE KhensuCopilotData
    HEADER_NAME KhensuCopilotGenreData.h
    SOURCES ${CMAKE_SOURCE_DIR}/resources/genre_norms/genre_targets.json)
target_link_libraries(KhensuCopilot PRIVATE KhensuCopilotGenreData)
if(TARGET KhensuCopilotARA)
    target_link_libraries(KhensuCopilotARA PRIVATE KhensuCopilotGenreData) # same sources, same data
endif()
