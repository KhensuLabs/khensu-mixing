// desktop-app/Source/ui/AiDetection/AiDetectionScreen.h
// AI Detection & Cleanup - the full-screen surface for AI/forensic artifact
// detection: what was detected, where in the song, the recording's identity
// (Chromaprint fingerprint + AcoustID), and cleanup options.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/KhensuErrorState.h"
#include "../shared/SegmentedControl.h"
#include "SourceSummaryView.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <memory>
#include <string>
#include <vector>

namespace khensu {
class IAppController;

class AiDetectionScreen final : public juce::Component, private juce::Timer {
  public:
    /// `baseUrl` points the screen at another service (tests: a fake one).
    explicit AiDetectionScreen(IAppController& c, std::string baseUrl = ipc::HttpServiceBridge::kDefaultBaseUrl);
    void paint(juce::Graphics& g) override;
    void resized() override;

    void runScan(); // §1/§2 - /analyze/artifacts_scan (auto on first show)
    juce::String statusTextForTesting() const { return statusLabel_.getText(); }

  private:
    struct Finding {
        juce::String kind, summary, evidence;
        juce::String evidenceClass;     // "provenance" | "heuristic" | "observation"
        float weight = 0.0f;            // detector signal strength
        float effectiveWeight = 0.0f;   // what it contributes (observations: 0)
        float t0 = -1.0f, t1 = -1.0f;   // < 0 == whole-mix
    };
    struct ServiceRow {
        juce::String line;              // "resembles X (20%)" / "X: named by provenance (95%)"
        float likelihood = 0.0f;
        bool fromProvenance = false;
    };
    struct TimelineWindow {
        float start = 0.0f, end = 0.0f, confidence = 0.0f;
    };
    struct ReliabilityBin {
        float confidence = 0.0f;
        float observedAccuracy = 0.0f;
        int count = 0;
    };

    void runFingerprint(); // §3

    // Progress affordance for the deep scan: the learned watermark tiers run
    // for minutes, so an elapsed-time + spinner replaces a silent "Scanning..."
    // that reads as a hang. Cleared (and the timer stopped) on completion/error.
    void timerCallback() override;
    void beginScanProgress();
    void endScanProgress();
    juce::String elapsedString() const; // "m:ss" since the scan started
    bool scanning_ = false;
    juce::int64 scanStartMs_ = 0;
    int spinnerPhase_ = 0;

    void paintWhatDetected(juce::Graphics& g, juce::Rectangle<int> area);
    void paintReliabilityDiagram(juce::Graphics& g, juce::Rectangle<int> area);
    void paintTimeline(juce::Graphics& g, juce::Rectangle<int> area);
    void paintIdentity(juce::Graphics& g, juce::Rectangle<int> area);
    void paintCleanup(juce::Graphics& g, juce::Rectangle<int> area);

    IAppController& controller_;
    std::string baseUrl_;

    juce::TextButton scanButton_{TRANS("Scan for AI / Artifacts")};
    juce::Label statusLabel_;

    juce::var scan_;                  // last scan result (verdict wording helpers)
    juce::String notRunLine_;         // detectors not run / errored (AID-12)
    std::vector<TimelineWindow> timeline_;
    std::vector<Finding> findings_;
    std::vector<ServiceRow> services_;
    std::vector<float> clipPositions_;
    float durationSec_ = 0.0f;
    int detectorsRun_ = 0;
    bool hasData_ = false;

    // Mix-quality model (pipeline #9) - fetched alongside the scan; opt-in, so
    // usually reports "not available" until a model is trained + enabled.
    bool mixQualityFetched_ = false;
    bool mixQualityAvailable_ = false;
    juce::String mixQualityBand_, mixQualityNote_;
    float mixQualityProb_ = -1.0f; // < 0 == withheld / unavailable
    std::vector<ReliabilityBin> mixQualityReliability_;
    float mixQualityEce_ = -1.0f;
    float mixQualityEceLimit_ = -1.0f;

    // §3 identity/provenance
    juce::TextButton lookupButton_{TRANS("Look Up (AcoustID)")};
    juce::String fpMethod_, fpDigest_, identityLine_, identityStatus_;
    // "Is this the same recording as that file?" - local chroma fingerprint
    // compare (/analyze/fingerprint_compare, audit §9.2). Nothing leaves the machine.
    juce::TextButton compareButton_{TRANS("Compare with another file...")};
    juce::String compareLine_;
    bool compareIsMatch_ = false;
    std::unique_ptr<juce::FileChooser> compareChooser_;
    void chooseCompareFile();
    void runCompare(const juce::String& pathA, const juce::File& other);

    // §4 cleanup
    juce::ComboBox leakPreset_;
    juce::TextButton applyLeak_{TRANS("Apply LeakGate")};
    juce::TextButton openRecreate_{TRANS("Recreate Sound...")};
    juce::TextButton openHumanize_{TRANS("Humanize...")};
    juce::String cleanupStatus_;

    KhensuErrorState errorState_;
    /// CON-8: Provenance & Source - the AI-detection page, or the Source view
    /// (the session's source scans), which covers the page below the title.
    SegmentedControl viewStrip_;
    SourceSummaryView sourceView_{controller_};
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AiDetectionScreen)
};
} // namespace khensu
