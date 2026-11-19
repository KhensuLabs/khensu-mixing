// desktop-app/Source/ui/Dashboard/PerformanceAnalysisPanels.h
// Dashboard cards for the router_performance.py routes that existed (with
// tests) but had no UI (audit 2026-11-06 §9.2):
//   GrooveTimingPanel     POST /analyze/groove_timing    (rhythm, near Drum Groove)
//   HarmonyTimelinePanel  POST /analyze/harmony_timeline (harmony, near Chords)
//   ReverbTailPanel       POST /analyze/reverb_tail      (space, near Depth Ladder)
//   StemPhasePanel        POST /analyze/stem_phase       (stems, near Sub-Bass Phase)
// All on demand (Run button): none is part of the results gate.
// THREAD_CONTEXT(UIThread)
#pragma once
#include "../shared/OnDemandScanPanel.h"
#include <vector>

namespace khensu {

/// Swing %, pocket (ahead / behind the grid) and timing spread, plus a
/// scatter of each onset's offset from the nearest grid line over time.
class GrooveTimingPanel final : public OnDemandScanPanel {
  public:
    explicit GrooveTimingPanel(IAppController& c);
    void paint(juce::Graphics& g) override;

  protected:
    void run() override;
    void clearResult() override;
    void layoutBody(juce::Rectangle<int> area) override;
    juce::String idleText() const override;

  private:
    ipc::HttpServiceBridge::GrooveTimingResult result_;
    juce::Rectangle<int> textArea_, plotArea_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrooveTimingPanel)
};

/// Chord progression as a timeline strip with key-change markers.
class HarmonyTimelinePanel final : public OnDemandScanPanel {
  public:
    explicit HarmonyTimelinePanel(IAppController& c);
    void paint(juce::Graphics& g) override;

  protected:
    void run() override;
    void clearResult() override;
    void layoutBody(juce::Rectangle<int> area) override;
    juce::String idleText() const override;

  private:
    ipc::HttpServiceBridge::HarmonyTimelineResult result_;
    double spanSec_ = 0.0;
    juce::Rectangle<int> stripArea_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HarmonyTimelinePanel)
};

/// Free-decay RT60 and the tail energy between hits.
class ReverbTailPanel final : public OnDemandScanPanel {
  public:
    explicit ReverbTailPanel(IAppController& c);
    void paint(juce::Graphics& g) override;

  protected:
    void run() override;
    void clearResult() override;
    void layoutBody(juce::Rectangle<int> area) override;
    juce::String idleText() const override;

  private:
    ipc::HttpServiceBridge::ReverbTailResult result_;
    juce::Rectangle<int> readoutArea_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReverbTailPanel)
};

/// Pairwise stem correlation / polarity. Enabled only with REAL separated
/// stems (Demucs) - the HPSS fallback split is not a source separation, so
/// its "stems" correlate by construction (AID-07).
class StemPhasePanel final : public OnDemandScanPanel {
  public:
    explicit StemPhasePanel(IAppController& c);
    void paint(juce::Graphics& g) override;
    void refreshAvailability() override;
    void visibilityChanged() override { refreshAvailability(); }

  protected:
    void run() override;
    void clearResult() override;
    void layoutBody(juce::Rectangle<int> area) override;
    juce::String idleText() const override;
    bool canRun() const override;

  private:
    ipc::HttpServiceBridge::StemPhaseResult result_;
    juce::Rectangle<int> rowsArea_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemPhasePanel)
};

} // namespace khensu
