// desktop-app/Source/ui/Dashboard/SongTimelineCard.cpp
#include "SongTimelineCard.h"
#include "../shared/PanelRunState.h"
#include "../shared/TimelineCache.h"
#include "../shared/TimelineMarkers.h"
#include <khensu/targets/GenreDatabase.h>
#include <set>

namespace khensu {

SongTimelineCard::SongTimelineCard(PanelCardController& controller)
    : PanelCard(controller, TRANS("Where in the song"))
{
    setExplainer(TRANS("Timeline"),
                 TRANS("Sections, the waveform, loudness over time, the spectrum and every finding that happens at "
                       "a moment, on one time axis. Click to play from a point; click a finding to hear it. Scroll "
                       "to zoom, drag to pan, double-click to see the whole track."));
    addAndMakeVisible(view_);
    view_.onSeek = [this](double sec) {
        controller_.playback().setPlaybackPosition(sec);
        if (!controller_.playback().isPlaying()) controller_.playback().playAudio();
    };
    view_.onPlayMoment = [this](double a, double b) { controller_.shell().loopRegion(a, b); };
    view_.onShowFinding = [this](const std::string& key) { controller_.shell().showIssue(key); };
    vblank_ = std::make_unique<juce::VBlankAttachment>(this, [this] { onVBlank(); });
    treeListener_ = listen(controller_.renders(), this, &IRenders::addRenderTreeListener,
                           &IRenders::removeRenderTreeListener, [this] { refresh(); });
    abListener_ = listen(controller_.playback(), this, &IPlayback::addAbRenderAppliedListener,
                         &IPlayback::removeAbRenderAppliedListener, [this] { refresh(); });
}

SongTimelineCard::~SongTimelineCard() = default;

void SongTimelineCard::resized()
{
    PanelCard::resized();
    view_.setBounds(bodyArea().reduced(KhensuTheme::sp3, KhensuTheme::sp2));
}

void SongTimelineCard::resetForSession()
{
    cache_.clear();
    pending_.clear();
    view_.setTimeline(nullptr);
    view_.setComparison(nullptr, {}, {});
    view_.setSections({});
    view_.setMarkers({});
    view_.setStatus(TRANS("The timeline appears when the track is loaded."));
}

void SongTimelineCard::onVBlank()
{
    if (!isShowing()) return;
    view_.setPlayhead(controller_.playback().getPlaybackDuration() > 0.0 ? controller_.playback().getPlaybackPosition()
                                                                        : -1.0);
}

SongTimelineCard::TimelinePtr SongTimelineCard::cached(const juce::String& path) const
{
    const auto it = cache_.find(path);
    return it != cache_.end() ? it->second : nullptr;
}

void SongTimelineCard::request(const juce::String& path)
{
    if (path.isEmpty() || cache_.count(path) != 0 || pending_.count(path) != 0) return;
    if (auto hit = timeline_cache::find(path)) {
        cache_[path] = hit;
        return;
    }
    pending_.insert(path);
    fetchQuietly(
        *this, controller_, [path] { return timeline_cache::measure(path); },
        [path](SongTimelineCard& self, TimelinePtr tl) {
            self.cache_[path] = tl != nullptr && !tl->empty() ? tl : nullptr;
            self.apply();
        },
        [path](SongTimelineCard& self) { self.pending_.erase(path); }, true);
}

void SongTimelineCard::refresh()
{
    if (!controller_.hasActiveSession()) {
        resetForSession();
        return;
    }
    const auto current = controller_.currentAuditionPath().isNotEmpty() ? controller_.currentAuditionPath()
                                                                        : controller_.activeSourcePath();
    request(current);
    const auto original = controller_.activeSourcePath();
    if (original != current) request(original);
    else if (const auto* s = controller_.getActiveSession(); s != nullptr && !s->target.reference_track_paths.empty())
        request(juce::String(juce::CharPointer_UTF8(s->target.reference_track_paths.front().c_str())));
    apply();
}

void SongTimelineCard::apply()
{
    const auto current = controller_.currentAuditionPath().isNotEmpty() ? controller_.currentAuditionPath()
                                                                        : controller_.activeSourcePath();
    const auto tl = cached(current);
    if (tl == nullptr) {
        view_.setTimeline(nullptr);
        view_.setStatus(pending_.count(current) != 0 ? TRANS("Measuring the timeline...")
                                                     : TRANS("The timeline could not be measured for this file."));
        return;
    }
    view_.setTimeline(tl);

    // VIZ-3a / VIZ-N1: the comparison line.
    const auto original = controller_.activeSourcePath();
    if (original != current && cached(original) != nullptr) {
        view_.setComparison(cached(original), TRANS("Original"), KhensuTheme::textSecondary);
    } else if (const auto* s = controller_.getActiveSession(); s != nullptr && !s->target.reference_track_paths.empty()) {
        const juce::String ref(juce::CharPointer_UTF8(s->target.reference_track_paths.front().c_str()));
        view_.setComparison(cached(ref), juce::File(ref).getFileNameWithoutExtension(), KhensuTheme::vizReference);
    } else {
        view_.setComparison(nullptr, {}, {});
    }

    std::vector<SectionData> sections;
    if (controller_.hasAnalysis() && controller_.currentAnalysis().section_analysis)
        sections = controller_.currentAnalysis().section_analysis->sections;
    view_.setSections(sections);

    std::optional<std::pair<float, float>> window;
    if (const auto* s = controller_.getActiveSession())
        if (const auto genre = controller_.genreDatabase().findById(s->target.primary_genre))
            window = std::make_pair(genre->lufs_range_min, genre->lufs_range_max);
    view_.setLoudnessWindow(window);
    view_.setMarkers(timeline_markers::build(controller_.issues().list(), tl.get(), sections));
}

} // namespace khensu
