add_executable(KhensuCoreTests
    shared-core/test_event_hub.cpp
    shared-core/test_version.cpp
    shared-core/test_session_model.cpp
    shared-core/test_plugin_capture.cpp
    shared-core/test_recommendation_schema.cpp
    shared-core/test_genre_database.cpp
    shared-core/test_dsp_math.cpp
    shared-core/test_core_misc.cpp
    shared-core/test_core_regressions.cpp
    shared-core/test_defect_benchmark.cpp  # MEAS-4 injected-defect benchmark
    shared-core/test_corpus_measure.cpp  # MEAS-1 corpus measurement ([.measure], hidden)
    shared-core/test_system_brain_database.cpp
    shared-core/test_loudness_analyzer.cpp
    shared-core/test_spectral_analyzer.cpp
    shared-core/test_stereo_analyzer.cpp
    shared-core/test_dynamics_analyzer.cpp
    shared-core/test_artifact_detector.cpp
    shared-core/test_tonal_fix_model.cpp  # MEAS-4
    shared-core/test_analysis_result_json.cpp  # WF-8
    shared-core/test_channel_layout.cpp
    shared-core/test_audio_analyzer_layout.cpp
    shared-core/test_ipc_bridge.cpp
    shared-core/test_service_identity.cpp
    shared-core/test_log_retention.cpp
    shared-core/test_target_interpolator.cpp
    shared-core/test_loudness_recommender.cpp
    shared-core/test_recommenders.cpp
    shared-core/test_hook_recommender.cpp
    shared-core/test_audio_file_prober.cpp
    shared-core/test_app_paths.cpp
    shared-core/test_recommendation_engine.cpp
    shared-core/test_mix_report.cpp
    shared-core/test_monitoring_reach.cpp  # NEW-10
    shared-core/test_mix_scorecard.cpp
    shared-core/test_viz_foundation.cpp  # VIZ-F1 / VIZ-1 / VIZ-P3
    shared-core/test_analysis_result.cpp
    shared-core/test_extended_recommenders.cpp
    shared-core/test_feedback_manager.cpp
    shared-core/test_user_preference_model.cpp
    shared-core/test_learned_preferences.cpp
    shared-core/test_plugin_scanner.cpp
    shared-core/test_crossover.cpp
    shared-core/test_vocal_macro_mapper.cpp
    shared-core/test_midi_ump_event.cpp
    shared-core/test_session_manager_migrations.cpp
    shared-core/test_audio_analyzer.cpp
    shared-core/test_audio_analyzer_golden.cpp
    shared-core/test_bad_audio_files.cpp
    shared-core/test_api_schema.cpp
    shared-core/test_credential_protector.cpp
    shared-core/test_secure_crypto.cpp
    shared-core/test_file_acl.cpp
    shared-core/test_service_health_tracker.cpp
    shared-core/test_recommendation_outbox.cpp
    shared-core/test_lookahead_limiter.cpp
    shared-core/test_offline_chain_renderer.cpp  # OVH-9 native chain renderer + parity
    shared-core/test_batch_result_aggregator.cpp
    shared-core/test_music_theory.cpp
    shared-core/test_music_theory_degree.cpp
    shared-core/test_perf_bench.cpp
    shared-core/test_orphan_route_parsers.cpp
    shared-core/test_native_meters_parity.cpp  # CE-PLT-3
    shared-core/test_collab_trust.cpp  # CE-TRU-3 evidence, CE-COL-1/2 cleanup
    shared-core/test_severity_scales.cpp  # FIX-9 reachable low-end rules, FIX-10 fixed severity scales
    shared-core/test_meter_ballistics.cpp  # UI-5 studio meter ballistics + stereo ring
    shared-core/test_recommendation_dsp_mapper.cpp  # OVH-8 findings -> plugin DSP
    shared-core/test_chain_preset.cpp  # FN2-5 shared chain preset format
)

target_link_libraries(KhensuCoreTests PRIVATE
    KhensuSharedCore
    Catch2::Catch2WithMain
    KhensuJuceTest   # shared with KhensuTests: JUCE compiled once (BLD-1)
)

# So test_session_manager_migrations.cpp can find storage/schema/*.sql
# regardless of ctest's working directory (audit ARCH-07 parity test).
target_compile_definitions(KhensuCoreTests PRIVATE
    KHENSU_REPO_ROOT_DIR="${CMAKE_SOURCE_DIR}"
)

khensu_apply_compiler_flags(KhensuCoreTests)

# Same long-path opt-in as the app, so the >260-char path test runs for real
# (it asks ntdll's RtlAreLongPathsEnabled to tell "not enabled" from broken).
if(WIN32)
    target_sources(KhensuCoreTests PRIVATE "${CMAKE_SOURCE_DIR}/cmake/KhensuLongPathAware.manifest")
endif()

# Auto-register discovered tests with CTest. PROPERTIES LABELS lets the suite
# be filtered by executable name (`ctest -L KhensuCoreTests`) - test names
# themselves are individual Catch2 case names, not the executable name, so
# `ctest -R KhensuCoreTests` alone matches nothing (frontend-review-2026-07-17.md P3).
catch_discover_tests(KhensuCoreTests PROPERTIES LABELS "KhensuCoreTests" SKIP_RETURN_CODE 4)

# ── UI tests (KhensuTests) ────────────────────────────────────────────────────
add_executable(KhensuTests
    ui/support/JuceGuiTestListener.cpp
    ui/ModalFocusTest.cpp
    ui/UiPaintBenchTest.cpp
    ui/SectionContrastTest.cpp
    ui/AutoScanCompletionTest.cpp
    ui/MultitrackModelTest.cpp
    ui/AlbumSequenceTest.cpp
    ui/KhensuThemeTest.cpp
    ui/OverhaulVerdictTest.cpp
    ui/OverhaulPlaybackTest.cpp
    ui/MotionFrameTest.cpp
    ui/GlassPanelTest.cpp
    ui/BadgeComponentTest.cpp
    ui/ZoneMeterTest.cpp
    ui/TargetRingGaugeTest.cpp
    ui/HeatSpectrumTest.cpp
    ui/StereoFieldVizTest.cpp
    ui/MetricTileTest.cpp
    ui/SegmentedControlTest.cpp
    ui/RecommendationCardTest.cpp
    ui/ServiceWaitPolicyTest.cpp
    ui/ListenerHandleTest.cpp
    ui/EvidenceReportBodyTest.cpp  # CE-TRU-1/3
    ui/NavItemTest.cpp
    ui/AnalysisScreenUx2Test.cpp  # UX2-1/3/4
    ui/IntakeLayoutTest.cpp
    ui/DashboardZone1Test.cpp
    ui/VocalWorkspaceTest.cpp
    ui/MixMasterTest.cpp
    ui/MixMasterRedesignTest.cpp   # UX2-2 / UX2-5 / FN2-4
    ui/SystemBrainTest.cpp
    ui/ExportScreenTest.cpp
    ui/OnlineSettingsTests.cpp
    ui/PaletteThemeSyncTest.cpp
    ui/VizPaletteTest.cpp  # VIZ-F2
    ui/SongTimelineTest.cpp  # VIZ-2
    ui/HiResSpectrogramTest.cpp  # VIZ-S1
    ui/VizFeaturesTest.cpp  # VIZ-3a/4b/5/N2/N5
    ui/ThemeReadabilityTest.cpp  # UX-16
    ui/RenderHistoryTest.cpp
    ui/PushToTalkTest.cpp  # CE-AG-4
    ui/AppControllerSplitTest.cpp
    ui/BackgroundRunnerTest.cpp
    ui/SessionServiceTest.cpp
    ui/PlaybackServiceTest.cpp
    ui/RenderServiceTest.cpp
    ui/NativeFixLoopTest.cpp  # OVH-9 analyse -> fix -> check without the service
    ui/ReportBodyTest.cpp
    ui/StemServiceTest.cpp
    ui/KeyedListenersTest.cpp
    ui/VocalEditServiceTest.cpp
    ui/MultitrackStemBusesTest.cpp
    ui/ProjectTracksTest.cpp
    ui/MasterAssistantTest.cpp
    ui/CardFindingRowTest.cpp
    ui/PluginCaptureInboxTest.cpp
    ui/ServiceLifecycleTest.cpp
    ui/CloudChatServiceTest.cpp
    ui/AnalysisServiceTest.cpp
    ui/AnalysisControllerTest.cpp
    ui/FixVerificationTest.cpp
    ui/IssueServiceTest.cpp
    ui/HeroSpectrumModelTest.cpp  # UI-1 / OVH-6
    ui/FixRefineTest.cpp  # NEW-4
    ui/IntakeGenreConfirmationTest.cpp
    ui/ServiceErrorHintTest.cpp
    ui/ModelDownloadsDisabledTest.cpp
    ui/ChainReasoningPlainLanguageTest.cpp
    ui/VocalUndoHistoryTest.cpp
    ui/SessionStateTest.cpp
    ui/AnalysisResultModelTest.cpp
    ui/RenderJobManagerTest.cpp
    ui/DecisionHashForwardingTest.cpp
    ui/DeliveryComplianceBridgeTest.cpp
    ui/CrashReporterTest.cpp
    ui/DiagnosticsBundleTest.cpp  # CE-ENG-4
    ui/UpdateNoticeTest.cpp
    ui/ServiceReadyWaitTest.cpp
    ui/AnalysisPendingOverlayTest.cpp
    ui/AbPreferenceNudgeTest.cpp
    ui/GuardrailTextTest.cpp
    ui/AiVerdictTextTest.cpp
    ui/MasteringAgentPanelTest.cpp
    ui/SectionIntensityPanelTest.cpp
    ui/ExportDeliveryPanelsTest.cpp
    ui/CeDeliveryPanelsTest.cpp  # CE-DEL-1..4
    ui/HttpBridgeDynamicJobRoutesTest.cpp
    ui/ServiceContractRegressionTest.cpp
    ui/PythonServiceManagerRestartTest.cpp
    ui/ServiceIdentityAndRenderPolicyTest.cpp
    ui/DeepAnalysisJobRunnerTest.cpp
    ui/ChatTargetResolverTest.cpp
    ui/SessionChainContextTest.cpp
    ui/WorkspaceRecoveryStateTest.cpp
    ui/SectionLufsParsingTest.cpp
    ui/ScheduledBackupTest.cpp
    ui/BackgroundReadKeyTest.cpp
    ui/AppControllerReadLifecycleHarnessTest.cpp
    ui/VocalEditingModelsTest.cpp
    ui/DesktopPluginChainTest.cpp
    ui/DesktopPluginChainIntegrationTest.cpp
    ui/MacroMappingEngineTest.cpp
    ui/EditViewStateTest.cpp
    ui/MixMapModelTest.cpp
    ui/OnboardingPlanTest.cpp
    ui/PanelDockTest.cpp
    ui/AccessibilityCoverageTest.cpp
    ui/ChordChartTest.cpp
    ui/MusicTheorySurfacingTest.cpp
    ui/TheoryUiTest.cpp
    ui/RenderSessionScopeTest.cpp
    ui/HeadlessCliTest.cpp
    ui/JobPollerTest.cpp
    ui/FeatureScreensTest.cpp
    ui/DesktopCoreRegressionsTest.cpp
    ui/OrphanRouteFeaturesTest.cpp
    ui/AssistantAgentTest.cpp  # CE-AG-1/2/4
    ui/ScreensCoverageTest.cpp  # VER-6
    ui/ServiceBackedScreensTest.cpp  # VER-6 (rest)
    ui/RealSongFindingsProbe.cpp  # manual: what fired on a real song ([.realsong], hidden)
    ui/FixChainMeasureTest.cpp  # MEAS-4: the fix chain per finding ([.fixchain], hidden)
    ui/FullPipelineMeasureTest.cpp  # FULL-1: every finding through the service ([.measurefull], hidden)
    ui/EssentialsSelectionTest.cpp  # UX-1 / VER-8
    ui/CopilotGroupsTest.cpp  # UX-13 / CUT-8
    ui/CoachMarkCardTest.cpp  # UX-15
    ui/IntakeTextTest.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/session/PlaybackEngine.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/session/SessionService.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/session/PlaybackService.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/session/RecommendationChain.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/session/RenderService.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/session/ReportBody.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/session/IssueService.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/session/StemService.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/session/MultitrackStemBuses.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/session/ProjectTracks.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/session/PluginCaptureInbox.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/session/VocalEditService.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/session/ServiceLifecycle.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/session/CloudChatService.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/session/AnalysisService.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/analysis/AnalysisController.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/CloudChatConsentController.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/export/AudioExporter.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/PythonServiceManager.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ServiceProcessJob.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/PortListeners.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/HeadlessCli.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/HeadlessCliProcess.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/RenderJobManager.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/analysis/DeepAnalysisJobRunner.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/Copilot/MasteringAgentPanel.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/Copilot/AutoMixPanel.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/Settings/LlmProviderPanel.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/NlCommandOverlay.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/VoiceRecorder.cpp  # CE-AG-4
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/Copilot/SectionIntensityPanel.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/PanelCard.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/IssueQueuePanel.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/HeroSpectrumView.cpp  # UI-1
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/SongTimelineView.cpp  # VIZ-2
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/HiResSpectrogramView.cpp  # VIZ-S1
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/FixDifferenceView.cpp  # VIZ-3a
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/LiveSpectrogramView.cpp  # VIZ-4a
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/StemViz.cpp  # VIZ-5
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/Dashboard/SongTimelineCard.cpp  # VIZ-2
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/SessionIntake/TargetSetupPanel.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/SessionIntake/TargetOverrideEditor.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/Milestones.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/ExportDelivery/BatchRenderPanel.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/ExportDelivery/PlatformMastersPanel.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/ExportDelivery/DeliveryQaPanel.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/ExportDelivery/DeliveryQaPanel_CeDelivery.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/ExportDelivery/DeliveryPackagePanel.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/ExportDelivery/ProjectBoardPanel.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/ExportDelivery/AlbumConsistencyView.cpp  # VIZ-N2
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/ExportDelivery/VisualReport.cpp  # VIZ-N5
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/ExportDelivery/MasterAssistantPanel.cpp
    ${CMAKE_SOURCE_DIR}/shared-core/src/ui/KhensuPalette.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/settings/OnlineSettings.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/KhensuTheme.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/KhensuLookAndFeel.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/PipelineProgressBar.cpp  # OVH-1 skipped stages
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/SessionIntake/DropZoneComponent.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/SessionIntake/MultitrackModel.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/SessionIntake/MaskingMatrixComponent.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/ZoneMeterComponent.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/TargetRingGauge.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/HeatSpectrumComponent.cpp
    # StereoFieldVizComponent.cpp needs a live AppController (whole-app link);
    # StereoFieldVizTest covers the Camera3D math it draws with instead.
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/MetricTileComponent.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/SegmentedControl.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/RecommendationCard.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/FindingCalibrationCache.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/ExportDelivery/EvidenceReportBody.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/ChainReasoningView.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/NavItemComponent.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/SidebarComponent.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/session/DesktopPluginChain.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/midi/MacroMappingEngine.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/ModalCardHost.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/AppNoticeBar.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/AnalysisPendingOverlay.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/CrashReporter.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/UpdateInstaller.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/diagnostics/DiagnosticsBundle.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/OnboardingWizard.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/PanelDockManager.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/AccessibilityCoverage.cpp
    # VER-6: the AI Detection, Reference Library and Settings screens
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/AiDetection/AiDetectionScreen.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/AiDetection/SourceSummaryView.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/ReferenceLibrary/ReferenceLibraryScreen.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/ReferenceLibrary/ReferenceProfilesBar.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/ReferenceLibrary/SimilarReferencesPanel.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/Settings/SettingsScreen.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/Settings/CalibrationPanel.cpp  # CUT-10
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/Settings/ComponentsPanel.cpp  # FEAT-10
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/Dashboard/GenreFitPanel.cpp  # NEW-3
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/Dashboard/MixCraftPanel.cpp  # UX2-1
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/Settings/MacroMappingPanel.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared/ModelDetailsDialog.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/midi/MacroControlRouter.cpp
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/midi/Midi2DeviceManager.cpp
)

# Provide a lightweight JuceHeader.h for the test executable.
# (juce_generate_juce_header only works with juce_add_* targets.)
target_include_directories(KhensuTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/juce_include
    ${CMAKE_SOURCE_DIR}/shared-core/include
    ${CMAKE_SOURCE_DIR}/desktop-app/Source
    ${CMAKE_SOURCE_DIR}/desktop-app/Source/ui/shared
)

target_compile_definitions(KhensuTests PRIVATE
    JUCE_STANDALONE_APPLICATION=0
    KHENSU_TESTS=1
    # JUCE_WEB_BROWSER / USE_CURL / MODAL_LOOPS_PERMITTED come from
    # KhensuJuceTest (cmake/KhensuJuce.cmake). RenderJobManagerTest pumps the
    # message loop with runDispatchLoopUntil(), which needs modal loops.
)

target_link_libraries(KhensuTests PRIVATE
    KhensuSharedCore
    Catch2::Catch2WithMain
    KhensuJuceTest   # JUCE_MODAL_LOOPS_PERMITTED=1 for runDispatchLoopUntil()
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

khensu_apply_compiler_flags(KhensuTests)
catch_discover_tests(KhensuTests PROPERTIES LABELS "KhensuTests" SKIP_RETURN_CODE 4)

# ---------------------------------------------------------------------------
# KhensuPluginTests - deterministic processor tests for the VST3 copilot
# (audits CQ-02 latency compensation, CQ-03 allocation freedom, ARCH-04
# retirement). Links the KhensuCopilot shared-code target, which carries the
# JucePlugin_* defines and the processor/editor sources.
# AllocationGuard.cpp replaces global operator new/delete binary-wide - kept
# in its own target so the counting operators never leak into KhensuTests.
add_executable(KhensuPluginTests
    plugin/AllocationGuard.cpp
    plugin/PluginLatencyCompTest.cpp
    plugin/PluginRetirementTest.cpp
    plugin/PluginAllocationTest.cpp
    plugin/CaptureRecorderTest.cpp
    plugin/PluginPersistenceTest.cpp
    plugin/CopilotRealtimeTelemetryTest.cpp
    plugin/CopilotTransportControllerTest.cpp
    plugin/CopilotServiceClientTest.cpp
    plugin/PluginPaletteTest.cpp
    plugin/PluginRealtimeStressTest.cpp
    plugin/PluginEditorAccessibilityTest.cpp
    plugin/PluginAutomationFuzzTest.cpp
    plugin/CopilotOutputStageTest.cpp
    # FIX-11 / FIX-12
    plugin/CopilotLoudnessParityTest.cpp
    plugin/CopilotOutputStageCeilingTest.cpp
    plugin/HostedChainPanelTest.cpp
    plugin/HostedChainManagerTest.cpp
    plugin/HostedChainRealPluginTest.cpp
    plugin/CopilotBackgroundWorkTest.cpp
    plugin/PluginvalStateRestorationTest.cpp
    plugin/CopilotMixHubTest.cpp
    plugin/PluginDspTest.cpp
    plugin/PluginControlTest.cpp
    # CE-DAW-3 / CE-DAW-4
    plugin/CopilotLiveAnalysisTest.cpp
    plugin/CopilotFixAutomationTest.cpp
    # CE-ENG-4
    plugin/PluginCrashHandlerTest.cpp
    # CON-1: the Suggestions tab shares the issue queue's state
    plugin/SuggestionIssueStateTest.cpp
    # UI-5 / UI-8 / UI-9 / OVH-8
    plugin/PluginDeEsserShelfTest.cpp
    plugin/PluginPresetStateTest.cpp
    plugin/PluginLiveFixEditorTest.cpp
    # UX2-6a / UX2-6b: knob readout + typed values, undo history
    plugin/PluginUndoHistoryTest.cpp
)

target_include_directories(KhensuPluginTests PRIVATE
    ${CMAKE_SOURCE_DIR}/plugin-copilot/Source
    # PluginProcessor.h includes <JuceHeader.h>; juce_generate_juce_header
    # writes it into the plugin's artefacts dir and does not propagate the
    # include path to targets that link the shared-code target.
    ${CMAKE_BINARY_DIR}/plugin-copilot/KhensuCopilot_artefacts/JuceLibraryCode
)

target_compile_definitions(KhensuPluginTests PRIVATE
    JUCE_STANDALONE_APPLICATION=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    # PluginvalStateRestorationTest hosts the built bundle through the real
    # VST3 wrapper; build it first so the test never runs a stale copy.
    "KHENSU_COPILOT_VST3_PATH=\"${CMAKE_BINARY_DIR}/plugin-copilot/KhensuCopilot_artefacts/$<CONFIG>/VST3/Khensu Copilot.vst3\""
)
add_dependencies(KhensuPluginTests KhensuCopilot_VST3)

target_link_libraries(KhensuPluginTests PRIVATE
    KhensuCopilot
    KhensuSharedCore
    Catch2::Catch2WithMain
    # Its own JUCE copy (compiled with JUCE_MODAL_LOOPS_PERMITTED=1 for
    # runDispatchLoopUntil), as before BLD-1 when it arrived through
    # KhensuSharedCore. The plugin's shared code keeps its own JUCE.
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

khensu_apply_compiler_flags(KhensuPluginTests COMPILES_JUCE)
get_target_property(_khensu_plugin_test_sources KhensuPluginTests SOURCES)
khensu_conversion_warnings(${_khensu_plugin_test_sources})
catch_discover_tests(KhensuPluginTests PROPERTIES LABELS "KhensuPluginTests" SKIP_RETURN_CODE 4)
