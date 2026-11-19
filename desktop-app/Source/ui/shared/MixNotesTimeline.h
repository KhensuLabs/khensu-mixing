// desktop-app/Source/ui/shared/MixNotesTimeline.h
// A colour-coded timeline strip of the time-anchored mix notes: each note that
// carries a time_range is a marker positioned by time and coloured by severity
// (red severe / amber moderate / blue minor / green strength). In the
// High-Contrast theme each severity also gets a distinct shape so it reads
// without relying on colour (accessibility). Hover shows the note; click seeks
// playback to it. THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <khensu/recommendations/Recommendation.h>
#include <vector>

namespace khensu {

class IAppController;

class MixNotesTimeline final : public juce::Component, public juce::TooltipClient, private juce::Timer {
  public:
    static constexpr int kHeight = 26;

    explicit MixNotesTimeline(IAppController& c);
    ~MixNotesTimeline() override;

    /// Keeps only the notes that carry a valid time_range.
    void setNotes(const std::vector<Recommendation>& recs);

    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent& e) override;
    juce::String getTooltip() override;

  private:
    void timerCallback() override;
    int markerAt(juce::Point<int> p) const; ///< index into notes_, or -1
    float xForTime(float sec) const;

    struct Marker {
        float startSec;
        float severity;
        bool isStrength;
        juce::String text;
    };

    IAppController& controller_;
    std::vector<Marker> notes_;
    double durationSec_ = 0.0;
    float playheadSec_ = -1.0f;
    int hoverIndex_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixNotesTimeline)
};

} // namespace khensu
