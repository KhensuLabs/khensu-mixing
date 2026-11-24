// desktop-app/Source/ui/Dashboard/SongTimelineCard.h
// VIZ-2: the Analysis screen's "where in the song" card - the SongTimelineView
// fed from the session: the native timeline of the version being auditioned
// (measured in the background, cached per file), the section analysis, the
// genre's loudness window and every finding placed where it happens
// (TimelineMarkers.h). When a fixed version is current the original's
// loudness is the dashed comparison line (VIZ-3a); otherwise the session's
// first reference track is (VIZ-N1). The playhead follows the transport
// and a click seeks (VIZ-2b).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <map>
#include <set>
#include <memory>
#include "../shared/ListenerHandle.h"
#include "../shared/PanelCard.h"
#include "../shared/SongTimelineView.h"

namespace khensu {

class SongTimelineCard final : public PanelCard {
  public:
    explicit SongTimelineCard(PanelCardController& controller);
    ~SongTimelineCard() override;

    /// Re-reads the session (analysis, issues, version) and loads what is missing.
    void refresh();
    void resetForSession() override;
    void resized() override;

    SongTimelineView& view() noexcept { return view_; }

  private:
    using TimelinePtr = std::shared_ptr<const TrackTimeline>;
    void request(const juce::String& path);
    TimelinePtr cached(const juce::String& path) const;
    void apply();
    void onVBlank();

    SongTimelineView view_;
    std::map<juce::String, TimelinePtr> cache_;
    std::set<juce::String> pending_;
    std::unique_ptr<juce::VBlankAttachment> vblank_;
    // After everything the callbacks touch (ListenerHandle's ordering rule).
    ListenerHandle treeListener_, abListener_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SongTimelineCard)
};

} // namespace khensu
