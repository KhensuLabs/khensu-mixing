option(KHENSU_ENABLE_WINDOWS_MIDI_SERVICES
       "Enable the optional Windows MIDI Services SDK backend for native MIDI 2.0"
       OFF)

set(KHENSU_NEEDS_WINDOWS_MIDI_SERVICES FALSE)
if(WIN32 AND KHENSU_ENABLE_WINDOWS_MIDI_SERVICES)
    set(KHENSU_NEEDS_WINDOWS_MIDI_SERVICES TRUE)
endif()

juce_add_gui_app(KhensuDesktop
    PRODUCT_NAME "Khensu Mixing"
    VERSION      "1.0.1"
    BUNDLE_ID    "com.khensu.mixing.desktop"
    COMPANY_NAME "Khensu"
    # The app icon from its 4K master (Explorer, taskbar, window; the macOS .icns);
    # scripts/make_app_icon.py draws it.
    ICON_BIG     "${CMAKE_SOURCE_DIR}/resources/icons/khensu_icon_4096.png"
    ICON_SMALL   "${CMAKE_SOURCE_DIR}/resources/icons/khensu_icon_256.png"
    MICROPHONE_PERMISSION_ENABLED TRUE  # CE-AG-4 push-to-talk
    MICROPHONE_PERMISSION_TEXT "Khensu Mixing records spoken mix instructions while you hold the push-to-talk button."
    NEEDS_WINDOWS_MIDI_SERVICES ${KHENSU_NEEDS_WINDOWS_MIDI_SERVICES}
)

juce_generate_juce_header(KhensuDesktop)

# Win32 paths over 260 characters (deep project folders, synced drives).
# Needs the machine policy LongPathsEnabled=1 as well - see the manifest.
if(WIN32)
    target_sources(KhensuDesktop PRIVATE "${CMAKE_SOURCE_DIR}/cmake/KhensuLongPathAware.manifest")
endif()

target_sources(KhensuDesktop PRIVATE
    Source/Main.cpp
    Source/MainWindow.cpp
    Source/AppController.cpp
    Source/AppController_Session.cpp
    Source/AppController_Render.cpp
    Source/AppController_Playback.cpp
    Source/AppController_Notices.cpp
    Source/AppController_Diagnostics.cpp
    Source/AppController_Guardrail.cpp
    Source/AppController_Onboarding.cpp
    Source/AppController_Reasoning.cpp
    Source/diagnostics/AppDiagnostics.cpp
    Source/AppController_Collab.cpp  # CE-COL-1 bundles + revision report
    Source/CrashReporter.cpp
    Source/UpdateInstaller.cpp
    Source/diagnostics/DiagnosticsBundle.cpp  # CE-ENG-4
    Source/session/PlaybackEngine.cpp
    Source/session/DesktopPluginChain.cpp
    Source/session/SessionService.cpp
    Source/session/PlaybackService.cpp
    Source/session/RecommendationChain.cpp
    Source/session/RenderService.cpp
    Source/session/ReportBody.cpp
    Source/session/IssueService.cpp
    Source/session/StemService.cpp
    Source/session/MultitrackStemBuses.cpp
    Source/session/ProjectTracks.cpp
    Source/session/PluginCaptureInbox.cpp
    Source/session/VocalEditService.cpp
    Source/session/ServiceLifecycle.cpp
    Source/session/CloudChatService.cpp
    Source/session/AnalysisService.cpp
    Source/PythonServiceManager.cpp
    Source/ServiceProcessJob.cpp
    Source/PortListeners.cpp
    Source/HeadlessCli.cpp
    Source/HeadlessCliProcess.cpp
    Source/RenderJobManager.cpp
    Source/analysis/AnalysisController.cpp
    Source/analysis/DeepAnalysisJobRunner.cpp
    Source/settings/OnlineSettings.cpp
    Source/midi/Midi2DeviceManager.cpp
    Source/midi/MacroMappingEngine.cpp
    Source/midi/MacroControlRouter.cpp
    Source/ui/Settings/SettingsScreen.cpp
    Source/ui/Settings/MacroMappingPanel.cpp
    Source/ui/Settings/LlmProviderPanel.cpp  # CE-AG-2
    Source/ui/AiDetection/AiDetectionScreen.cpp
    Source/ui/AiDetection/SourceSummaryView.cpp
    Source/ui/shared/KhensuTheme.cpp
    Source/ui/shared/KhensuLookAndFeel.cpp
    Source/ui/shared/ModalCardHost.cpp
    Source/ui/shared/AppNoticeBar.cpp
    Source/ui/shared/AnalysisPendingOverlay.cpp
    Source/ui/shared/ExplainerPopup.cpp
    Source/ui/shared/ExplainerOverlay.cpp
    Source/ui/shared/OnboardingWizard.cpp
    Source/ui/shared/PanelDockManager.cpp
    Source/ui/shared/AccessibilityCoverage.cpp
    Source/ui/shared/ChainReasoningView.cpp
    Source/ui/shared/ChainReasoningOverlay.cpp
    Source/ui/shared/MetricDiffTable.cpp
    Source/ui/shared/MetricDiffOverlay.cpp
    Source/ui/shared/MetricDiffChart.cpp
    Source/ui/shared/AbxTrialOverlay.cpp
    Source/ui/shared/CommandPaletteOverlay.cpp
    Source/ui/shared/CommandPaletteController.cpp
    Source/ui/shared/NlCommandOverlay.cpp  # CE-AG-4
    Source/ui/shared/VoiceRecorder.cpp  # CE-AG-4 push-to-talk
    Source/ui/shared/CloudChatConsentController.cpp
    Source/ui/shared/DataLifecycleController.cpp
    Source/ui/shared/BadgeComponent.cpp
    Source/ui/shared/MeterBarComponent.cpp
    Source/ui/shared/ZoneMeterComponent.cpp
    Source/ui/shared/TargetRingGauge.cpp
    Source/ui/shared/LiveLufsMeterComponent.cpp
    Source/ui/shared/RadarChartComponent.cpp
    Source/ui/shared/SpectrumComponent.cpp
    Source/ui/shared/HeatSpectrumComponent.cpp
    Source/ui/shared/SpectrogramImage.cpp
    Source/ui/shared/TimelineComponent.cpp
    Source/ui/shared/PlaybackTransportBar.cpp
    Source/ui/shared/GoniometerComponent.cpp
    Source/ui/shared/SonicOrbitComponent.cpp
    Source/ui/shared/MultiBandGoniometerComponent.cpp
    Source/ui/shared/ModelDownloadDialog.cpp
    Source/ui/shared/ModelDetailsDialog.cpp
    Source/ui/shared/StereoFieldVizComponent.cpp
    Source/ui/shared/PanelCard.cpp
    Source/ui/shared/IssueQueuePanel.cpp
    Source/ui/shared/SessionCommentsLane.cpp  # CE-COL-1
    Source/ui/shared/RevisionReportView.cpp  # CE-COL-1 / FEAT-9
    Source/ui/shared/RecommendationCard.cpp
    Source/ui/shared/FindingCalibrationCache.cpp  # CE-TRU-1
    Source/ui/shared/ListeningRoomPanel.cpp  # CE-COL-3
    Source/ui/shared/CounterfactualView.cpp  # CE-TRU-2
    Source/ui/shared/ExplainMomentView.cpp  # CE-AI-6
    Source/ui/shared/MetricTileComponent.cpp
    Source/ui/shared/SegmentedControl.cpp
    Source/ui/shared/ShimmerComponent.cpp
    Source/ui/shared/NavItemComponent.cpp
    Source/ui/shared/SidebarComponent.cpp
    Source/ui/shared/RenderQueueStrip.cpp
    Source/ui/shared/SessionHealthStrip.cpp
    Source/ui/shared/PipelineProgressBar.cpp
    Source/ui/SessionIntake/IntakeScreen.cpp
    Source/ui/SessionIntake/NativeMeterTiles.cpp  # CE-PLT-3
    Source/ui/SessionIntake/IntakeQueueList.cpp
    Source/ui/SessionIntake/IntakeBatchReport.cpp
    Source/ui/SessionIntake/DropZoneComponent.cpp
    Source/ui/SessionIntake/FileLoadingPanel.cpp
    Source/ui/SessionIntake/TargetSetupPanel.cpp
    Source/ui/SessionIntake/RecentSessionsPanel.cpp
    Source/ui/SessionIntake/TrendLinePanel.cpp
    Source/ui/SessionIntake/TargetOverrideEditor.cpp
    Source/ui/SessionIntake/MultitrackModel.cpp
    Source/ui/SessionIntake/MaskingMatrixComponent.cpp
    Source/ui/SessionIntake/MultitrackSessionView.cpp
    Source/ui/Dashboard/DashboardScreen.cpp
    Source/ui/Dashboard/SummaryPanel.cpp
    Source/ui/Dashboard/LoudnessPanel.cpp
    Source/ui/Dashboard/DynamicsPanel.cpp
    Source/ui/Dashboard/SpectralPanel.cpp
    Source/ui/Dashboard/StereoFieldPanel.cpp
    Source/ui/Dashboard/PhasePanel.cpp
    Source/ui/Dashboard/SubBassPhasePanel.cpp
    Source/ui/Dashboard/StreamingNormalizationPanel.cpp
    Source/ui/Dashboard/SourceForensicsPanel.cpp
    Source/ui/Dashboard/TuningReferencePanel.cpp
    Source/ui/Dashboard/MonoCompatPanel.cpp
    Source/ui/Dashboard/MidSideSpectrogramPanel.cpp
    Source/ui/Dashboard/PhaseHeatmapPanel.cpp
    Source/ui/Dashboard/StemEnergyTimelinePanel.cpp
    Source/ui/Dashboard/ReferenceSpectralDeltaPanel.cpp
    Source/ui/Dashboard/RecreateSoundPanel.cpp
    Source/ui/Dashboard/ArtifactPanel.cpp
    Source/ui/Dashboard/TranslationPanel.cpp
    Source/ui/Dashboard/MixNotesPanel.cpp
    Source/ui/shared/MixNotesTimeline.cpp
    Source/ui/Dashboard/SonicFingerprintPanel.cpp
    Source/ui/Dashboard/HookDashboard.cpp
    Source/ui/Dashboard/MemorabilityPanel.cpp
    Source/ui/Dashboard/StructuralFlowPanel.cpp
    Source/ui/Dashboard/HeavinessPanel.cpp
    Source/ui/Dashboard/ReferenceComparePanel.cpp
    Source/ui/Dashboard/EqCurvePanel.cpp
    Source/ui/Dashboard/LoudnessArcPanel.cpp
    Source/ui/Dashboard/SongInfoPanel.cpp
    Source/ui/Dashboard/VocalInfoPanel.cpp
    Source/ui/Dashboard/EmotionalArcPanel.cpp
    Source/ui/Dashboard/BenchmarkPanel.cpp
    Source/ui/Dashboard/GenreDistributionPanel.cpp
    Source/ui/Dashboard/SpectrogramPanel.cpp
    Source/ui/Dashboard/WaveformEditorPanel.cpp
    Source/ui/Dashboard/SpatialAudioPanel.cpp
    Source/ui/Dashboard/DrumPatternPanel.cpp
    Source/ui/Dashboard/LoudnessCurvePanel.cpp
    Source/ui/Dashboard/TonalBalancePanel.cpp
    Source/ui/Dashboard/SourceTriagePanel.cpp
    Source/ui/Dashboard/SignalIntegrityPanel.cpp
    Source/ui/Dashboard/ArrangementPanel.cpp
    Source/ui/Dashboard/DepthLadderPanel.cpp
    Source/ui/Dashboard/PerformanceAnalysisPanels.cpp
    Source/ui/Dashboard/PanBalancePanel.cpp
    Source/ui/Settings/CalibrationPanel.cpp
    Source/ui/Settings/ComponentsPanel.cpp  # FEAT-10
    Source/ui/MixMasterEngine/StemRebalancePanel.cpp  # FEAT-4
    Source/ui/Dashboard/GenreFitPanel.cpp  # NEW-3
    Source/ui/Dashboard/SectionReadinessPanel.cpp
    Source/ui/Dashboard/TaxonomyEnergyPanel.cpp
    Source/ui/Dashboard/TaxonomyMaskingPanel.cpp
    Source/ui/Dashboard/AiArtifactsPanel.cpp
    Source/ui/MixMasterEngine/HumanizePanel.cpp
    Source/ui/MixMasterEngine/WhatChangedPanel.cpp
    Source/ui/VocalWorkspace/StemEnergyPanel.cpp
    Source/ui/VocalWorkspace/VocalRenderPanel.cpp
    Source/ui/Dashboard/ScoreCardPanel.cpp
    Source/ui/Dashboard/MixCraftPanel.cpp
    Source/ui/Dashboard/ReadinessBreakdownBar.cpp
    Source/ui/Dashboard/MultiLaneHeatmap.cpp
    Source/ui/Dashboard/IssueHeatmapPanel.cpp
    Source/ui/Dashboard/WeightProfileOverlay.cpp
    Source/ui/Dashboard/DashboardLayoutPanel.cpp
    Source/ui/VocalWorkspace/VocalWorkspaceScreen.cpp
    Source/ui/VocalWorkspace/VocalAnalysisPanel.cpp
    Source/ui/VocalWorkspace/VocalStrategyPanel.cpp
    Source/ui/VocalWorkspace/VocalMacroPanel.cpp
    Source/ui/VocalWorkspace/VocalHookPanel.cpp
    Source/ui/VocalWorkspace/VocalQuickStatsPanel.cpp
    Source/ui/VocalWorkspace/StemMaskingPanel.cpp
    Source/ui/VocalWorkspace/LyricIntelligibilityPanel.cpp
    Source/ui/VocalWorkspace/GainRiderPanel.cpp
    Source/ui/VocalWorkspace/PreflightPanel.cpp
    Source/ui/VocalWorkspace/CompingPanel.cpp
    Source/ui/VocalWorkspace/PitchAccuracyPanel.cpp
    Source/ui/VocalWorkspace/PitchNoteGridEditor.cpp
    Source/ui/VocalWorkspace/WordClipGainEditor.cpp
    Source/ui/VocalWorkspace/TakeAlignmentPanel.cpp
    Source/ui/VocalWorkspace/VocalHarmonyPanel.cpp
    Source/ui/VocalWorkspace/MixMapCanvas.cpp
    Source/ui/MixMasterEngine/MixMasterScreen.cpp
    Source/ui/MixMasterEngine/SuggestionListPanel.cpp
    Source/ui/MixMasterEngine/GainStagingPanel.cpp
    Source/ui/MixMasterEngine/StreamingImpactPanel.cpp
    Source/ui/MixMasterEngine/ProvenanceTimelinePanel.cpp
    Source/ui/MixMasterEngine/SnapshotHistoryPanel.cpp
    Source/ui/MixMasterEngine/SignalFlowRackPanel.cpp
    Source/ui/MixMasterEngine/CompressionXrayPanel.cpp
    Source/ui/MixMasterEngine/RenderTreePanel.cpp
    Source/ui/MixMasterEngine/PresetLibraryPanel.cpp
    Source/ui/Copilot/CopilotScreen.cpp
    Source/ui/Copilot/MixerChainPanel.cpp
    Source/ui/Copilot/HostedChainPanel.cpp
    Source/ui/Copilot/MasteringAgentPanel.cpp
    Source/ui/Copilot/SectionIntensityPanel.cpp
    Source/ui/Copilot/TasteProfilePanel.cpp
    Source/ui/Copilot/FeedbackDecoderPanel.cpp
    Source/ui/Copilot/AutoMixPanel.cpp  # CE-AG-1
    Source/export/AudioExporter.cpp
    Source/ui/ExportDelivery/FormatPanel.cpp
    Source/ui/ExportDelivery/AlbumAssemblyPanel.cpp
    Source/ui/ExportDelivery/ProjectBoardPanel.cpp
    Source/ui/ExportDelivery/MasterAssistantPanel.cpp
    Source/ui/ExportDelivery/AlbumMasterDialog.cpp
    Source/ui/ExportDelivery/BatchRenderPanel.cpp
    Source/ui/ExportDelivery/PlatformMastersPanel.cpp
    Source/ui/ExportDelivery/DeliveryQaPanel.cpp
    Source/ui/ExportDelivery/DeliveryQaPanel_CeDelivery.cpp  # CE-DEL-1/3
    Source/ui/ExportDelivery/DeliveryPackagePanel.cpp        # CE-DEL-2/4
    Source/ui/ExportDelivery/ReportPanel.cpp
    Source/ui/ExportDelivery/EvidenceReportBody.cpp  # CE-TRU-3
    Source/ui/ExportDelivery/ExportScreen.cpp
    Source/ui/SystemBrain/SystemBrainPanel.cpp
    Source/ui/SystemBrain/FeatureCardComponent.cpp
    Source/ui/SystemBrain/FeatureDetailPanel.cpp
    Source/ui/Copilot/ChatConsolePanel.cpp
    Source/ui/Copilot/HarmonicContentBarChart.cpp
    Source/ui/Copilot/GainStagingWaterfallChart.cpp
    Source/ui/Issues/IssuesScreen.cpp
    Source/ui/shared/Milestones.cpp
    Source/ui/ReferenceLibrary/ReferenceLibraryScreen.cpp
    Source/ui/ReferenceLibrary/ReferenceProfilesBar.cpp  # CE-COL-4
    Source/ui/ReferenceLibrary/SimilarReferencesPanel.cpp  # CE-AI-1
)

target_link_libraries(KhensuDesktop PRIVATE
    KhensuSharedCore
    KhensuJuceApp   # the one compiled copy of JUCE for the app (BLD-1)
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

target_compile_definitions(KhensuDesktop PRIVATE
    # JUCE_WEB_BROWSER / USE_CURL / USE_LAME_AUDIO_FORMAT / PLUGINHOST_* come
    # from KhensuJuceApp, so the app and its JUCE copy can't disagree.
    JUCE_APPLICATION_IS_STANDALONE=1
    # JUCE_DISPLAY_SPLASH_SCREEN=0  # Requires paid JUCE license. Leave commented under Personal tier.
)

khensu_apply_compiler_flags(KhensuDesktop)

# BLD-2: every app source pulls in JuceHeader.h (and through it most of JUCE)
# - precompile it once. Off for a build that wants to check include hygiene.
option(KHENSU_PCH "Precompile JuceHeader.h and the common standard headers" ON)
if(KHENSU_PCH)
    target_precompile_headers(KhensuDesktop PRIVATE
        <JuceHeader.h> <algorithm> <functional> <map> <memory> <optional> <string> <vector>)
endif()

# Bundled data files go beside the executable - except in a macOS .app,
# where a loose file in Contents/MacOS fails code signing ("code object is
# not signed at all"); there they go in Contents/Resources, where
# BundledResources.h looks for them.
if(APPLE)
    set(KHENSU_APP_DATA_DIR "$<TARGET_BUNDLE_CONTENT_DIR:KhensuDesktop>/Resources")
else()
    set(KHENSU_APP_DATA_DIR "$<TARGET_FILE_DIR:KhensuDesktop>")
endif()

add_custom_command(TARGET KhensuDesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources/genre_norms"
        "${KHENSU_APP_DATA_DIR}/resources/genre_norms"
    VERBATIM)

# NEW-2: each finding's measured false-alarm rate and sensitivity.
add_custom_command(TARGET KhensuDesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${KHENSU_APP_DATA_DIR}/resources/calibration"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/resources/calibration/finding_evidence.json"
        "${KHENSU_APP_DATA_DIR}/resources/calibration/finding_evidence.json"
    VERBATIM)

add_custom_command(TARGET KhensuDesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources/system_brain"
        "${KHENSU_APP_DATA_DIR}/resources/system_brain"
    VERBATIM)

# Aurora Glass design fonts (Space Grotesk + JetBrains Mono, SIL OFL 1.1) -
# loaded as loose files by KhensuTheme, same convention as the two copies above.
add_custom_command(TARGET KhensuDesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources/fonts"
        "${KHENSU_APP_DATA_DIR}/resources/fonts"
    VERBATIM)
