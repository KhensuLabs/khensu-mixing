// desktop-app/Source/ui/Dashboard/LoudnessPanel.h
#pragma once
#include "../shared/PanelCard.h"
#include "../shared/ListenerHandle.h"
#include "../shared/ZoneMeterComponent.h"
#include "../shared/BadgeComponent.h"
#include "../shared/KhensuTheme.h"
#include "../shared/CompareDeltaChip.h"
#include "../shared/LiveLufsMeterComponent.h"
#include "../shared/KhensuSkeleton.h"
#include "../shared/KhensuEmptyState.h"
#include <khensu/analysis/AnalysisResult.h>
#include <vector>
#include <algorithm>
#include <cmath>

namespace khensu {

// Compact horizontal bar chart showing per-section integrated LUFS.
class SectionLoudnessBar : public juce::Component {
  public:
    SectionLoudnessBar() = default;
    void setSections(const std::vector<float>& lufsValues, float refLufs)
    {
        values_ = lufsValues;
        refLufs_ = refLufs;
        repaint();
    }
    void paint(juce::Graphics& g) override;

  private:
    std::vector<float> values_;
    float refLufs_ = -14.0f;
    JUCE_DECLARE_NON_COPYABLE(SectionLoudnessBar)
};

class LoudnessPanel final : public PanelCard {
  public:
    explicit LoudnessPanel(IAppController& c);
    ~LoudnessPanel() override;
    void setData(const AnalysisResult& r);
    void resized() override;

    /// App-wide Compare toggle: shows/hides the "vs ref" delta chip when a
    /// reference track's deltas are available (see CompareDeltaChip).
    void setCompareModeEnabled(bool enabled);

  private:
    ZoneMeterComponent lufsMeter_, lraMeter_;
    BadgeComponent plrBadge_, truePeakBadge_;
    SectionLoudnessBar sectionArc_;
    // Dedicated real-time row: the integrated-LUFS meter no longer gets
    // overwritten with short-term values (which silently changed its meaning
    // and stuck at the last live value when playback stopped). Bar meters +
    // a rolling momentary-LUFS distribution histogram, not just a text readout.
    LiveLufsMeterComponent liveMeter_;
    bool hasAnalysis_ = false;

    KhensuSkeleton skeleton_;
    KhensuEmptyState emptyState_;
    void updateStateVisibility();

    CompareDeltaChip compareChip_;
    bool compareEnabled_ = false;
    std::optional<ReferenceDeltas> lastRefDeltas_;
    void updateCompareChip();
    // ENG-2: hub registrations; destroyed (deregistered) before the members
    // above them that the callbacks touch.
    ListenerHandle realTimeMeterListener_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoudnessPanel)
};

} // namespace khensu
