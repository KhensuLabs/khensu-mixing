# ENG-9: the shared code is four libraries, each depending only on the ones
# below it: KhensuCommon (paths, credentials, HMAC) <- KhensuDsp (meters,
# analyzers, genre targets, the recommendation engine; no database, no
# network) <- KhensuServiceClient (the service bridge) <- KhensuData (the
# SQLite session / feedback store, the System Brain, plugin scanning).
# KhensuSharedCore links them all for the app, the plugin and the tests;
# a target that needs only the DSP links KhensuDsp.
add_library(KhensuCommon STATIC
    src/common/AppPaths.cpp
    src/common/CredentialProtector.cpp
    src/common/FileAcl.cpp
    src/common/HmacSha256.cpp
    src/common/SecureRandom.cpp
    src/presets/ChainPreset.cpp  # FN2-5 shared chain preset format
)

add_library(KhensuDsp STATIC
    src/session/AudioFileProber.cpp
    src/dsp/LookaheadBrickwallLimiter.cpp
    src/dsp/IirDesign.cpp  # OVH-9: the native chain renderer
    src/dsp/TruePeakOversampler.cpp
    src/dsp/ChainProcessor.cpp
    src/dsp/RealFft.cpp  # FN2-2a: native denoise / saturation stages
    src/dsp/SpectralDenoiser.cpp
    src/dsp/TapeSaturator.cpp
    src/dsp/OfflineChainRenderer.cpp
    src/dsp/DemoSong.cpp  # VIZ-U2 / VIZ-F3 demo song
    src/targets/GenreDatabase.cpp
    src/targets/GenreSpread.cpp  # VIZ-1 measured spread
    src/analysis/KWeighting.cpp
    src/analysis/TruePeakFilter.cpp  # MEAS-5
    src/analysis/TrackTimeline.cpp  # VIZ-2 native timeline
    src/analysis/LoudnessAnalyzer.cpp
    src/analysis/SpectralAnalyzer.cpp
    src/analysis/DynamicsAnalyzer.cpp
    src/analysis/StereoAnalyzer.cpp
    src/analysis/ArtifactDetector.cpp
    src/analysis/AudioAnalyzer.cpp
    src/analysis/ChannelLayout.cpp
    src/analysis/BatchResultAggregator.cpp
    src/analysis/AnalysisResultJson.cpp  # WF-8
    src/analysis/MusicTheory.cpp
    src/analysis/NativeMeters.cpp  # CE-PLT-3
    src/recommendations/RecommendationEngine.cpp
    src/recommendations/RecommendationEvidence.cpp
    src/recommendations/MixReportBuilder.cpp
    src/recommendations/MixScorecard.cpp
    src/recommendations/SpatialAudioAssessment.cpp
    src/midi/MidiUmpEvent.cpp
    src/ui/EqCurveComponent.cpp
    src/ui/KhensuPalette.cpp
    src/ui/StudioMeters.cpp  # UI-5 studio meters
    src/ui/KhensuKnob.cpp  # UI-5 knob / linear slider
    src/ui/ChartKit.cpp  # VIZ-F1 chart toolkit
    src/ui/ModuleViz.cpp  # VIZ-6 module visuals
)

add_library(KhensuServiceClient STATIC
    src/ipc/IpcTypes.cpp
    src/ipc/HttpServiceBridge.cpp
    src/ipc/HttpConnectionLimit.cpp
    src/ipc/HttpServiceBridge_Analysis.cpp
    src/ipc/HttpServiceBridge_Copilot.cpp
    src/ipc/HttpServiceBridge_Updates.cpp
    src/ipc/HttpServiceBridge_Delivery.cpp
    src/ipc/HttpServiceBridge_Performance.cpp
    src/ipc/HttpServiceBridge_Multitrack.cpp
    src/ipc/HttpServiceBridge_Album.cpp
    src/ipc/HttpServiceBridge_Collab.cpp  # CE-COL-1..4, CE-TRU-1/3
    src/ipc/HttpServiceBridge_Ml.cpp
    src/ipc/HttpServiceBridge_CeDelivery.cpp  # CE-DEL-1..4
    src/ipc/HttpServiceBridge_Intelligence.cpp  # CE-AI-1/5/6, CE-TRU-2
    src/ipc/HttpServiceBridge_Agent.cpp  # CE-AG-1/2/4
    src/ipc/generated/ApiSchema.cpp
)

add_library(KhensuData STATIC
    src/session/SessionManager.cpp
    src/session/OutboxDispatcher.cpp
    src/systembrain/SystemBrainDatabase.cpp
    src/feedback/FeedbackManager.cpp
    src/feedback/LearnedPreferences.cpp
    src/mixerchain/DspChainMapper.cpp
    src/mixerchain/VocalMacroMapper.cpp
    src/mixerchain/PluginScanner.cpp
    src/mixerchain/RecommendationDspMapper.cpp  # OVH-8 findings -> plugin DSP
)

set(KHENSU_CORE_LIBS KhensuCommon KhensuDsp KhensuServiceClient KhensuData)
add_library(KhensuSharedCore INTERFACE)
target_link_libraries(KhensuSharedCore INTERFACE KhensuData)


foreach(core_lib IN LISTS KHENSU_CORE_LIBS)
    target_include_directories(${core_lib}
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
        # FeedbackManager.h and friends are seen by DSP headers (inline helpers
        # that are only compiled where called); SQLite itself links in KhensuData.
        PUBLIC  $<TARGET_PROPERTY:SQLiteCpp,INTERFACE_INCLUDE_DIRECTORIES>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
        # Generated khensu_migrations.inc (audit ARCH-07 - see cmake/GenerateMigrations.cmake).
        PRIVATE ${KHENSU_GENERATED_INCLUDE_DIR}
    )

    # JUCE headers only (BLD-1): linking the INTERFACE module targets would
    # compile JUCE into this library *and* hand the sources to every consumer.
    # The final binary supplies the one compiled copy - KhensuJuceApp /
    # KhensuJuceTest (cmake/KhensuJuce.cmake) or the plugin's own modules.
    khensu_juce_headers(${core_lib} PUBLIC
        juce_audio_processors
        juce_audio_basics
        juce_audio_formats
        juce_core
        juce_dsp
        juce_gui_basics
    )

    if(APPLE)
        target_link_libraries(${core_lib} PRIVATE "-framework Security" "-framework CoreFoundation")
    endif()

    target_compile_definitions(${core_lib}
        PUBLIC
            JUCE_STANDALONE_APPLICATION=0
            JUCE_PLUGINHOST_VST3=1
            JUCE_PLUGINHOST_AU=1
        PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
    )

    khensu_apply_compiler_flags(${core_lib})
    # The generated OpenAPI client (scripts/gen_cpp_api.py) has one function per
    # schema; since the 0.2.88 route wave its Debug object exceeds MSVC's default
    # section limit (C1128). Release inlining keeps it under, which is why only
    # the Debug tree (verify_all's debug-capture) failed.
    if(MSVC)
        set_source_files_properties(src/ipc/generated/ApiSchema.cpp PROPERTIES COMPILE_OPTIONS /bigobj)
    endif()
endforeach()

target_link_libraries(KhensuDsp PUBLIC KhensuCommon)
target_link_libraries(KhensuServiceClient PUBLIC KhensuDsp)
target_link_libraries(KhensuData PUBLIC KhensuServiceClient SQLiteCpp)
