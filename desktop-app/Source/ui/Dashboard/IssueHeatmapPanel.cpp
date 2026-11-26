// desktop-app/Source/ui/Dashboard/IssueHeatmapPanel.cpp
#include "IssueHeatmapPanel.h"
#include "../shared/PanelRunState.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/DspDomainClassifier.h"
#include "../../IAppController.h"
#include "../shared/ChartKitDesktop.h"
#include "../shared/FloatVector.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <cmath>
#include <limits>

namespace khensu {

IssueHeatmapPanel::IssueHeatmapPanel(IAppController& c) : PanelCard(c, "Issue Heatmap"), heatmap_(&c.playback())
{
    setExplainer(TRANS("Issue Heatmap"), TRANS("Unified, transport-synced timeline. A shared time axis and playhead run "
                                  "across every lane: the spectro-temporal heatmap, an issue-density row, "
                                  "continuous loudness (LUFS) and stereo-correlation plots, and one "
                                  "recommendation-marker lane per domain (Spectral / Dynamics / Stereo). "
                                  "Click anywhere to play from there; hover to read values at that moment."));

    // VIS-1: the heatmap is a ChartKit chart - click-to-play, the playhead and
    // the screen-reader summary (built from its data) are its own.
    heatmap_.setDensityEnabled(true);
    addAndMakeVisible(heatmap_);
}

void IssueHeatmapPanel::setData(const AnalysisResult& r)
{
    const float span =
        controller_.playback().getPlaybackDuration() > 0.0 ? static_cast<float>(controller_.playback().getPlaybackDuration()) : 180.0f;

    // Stereo correlation over time - already in AnalysisResult (no fetch).
    stereoCorrelation_.clear();
    if (r.stereo_field_timeline.has_value()) {
        for (const auto& p : r.stereo_field_timeline->points)
            stereoCorrelation_.push_back({p.progress_fraction * span, p.correlation});
    }

    // The shared classifier (khensu::DspDomain). This heatmap only has lanes
    // for 3 of the 5 domains; Clip Guard/Input and Loudness/Output
    // recommendations don't appear here (the Zone 1 metric tiles cover them).
    std::vector<Recommendation> spectral, dynamics, stereo;
    for (const auto& rec : r.recommendations) {
        switch (classifyDspDomain(rec)) {
        case DspDomain::Spectral:
            spectral.push_back(rec);
            break;
        case DspDomain::Dynamics:
            dynamics.push_back(rec);
            break;
        case DspDomain::Stereo:
            stereo.push_back(rec);
            break;
        default:
            break;
        }
    }

    heatmap_.setSpan(span);
    heatmap_.setLanes({
        {TRANS("Spectral"), std::move(spectral)},
        {TRANS("Dynamics"), std::move(dynamics)},
        {TRANS("Stereo"), std::move(stereo)},
    });

    rebuildDataLanes();
    fetchSpectrogramIfNeeded();
    fetchLoudnessCurveIfNeeded();
}

void IssueHeatmapPanel::rebuildDataLanes()
{
    std::vector<TimelineDataLane> lanes;

    if (!lufsCurve_.empty()) {
        TimelineDataLane loud;
        loud.label = TRANS("Loudness");
        loud.suffix = " LUFS";
        loud.colour = KhensuTheme::vizCategory(0); // VIS-4: a series colour
        float lo = 0.0f, hi = -120.0f;
        loud.samples.reserve(lufsCurve_.size());
        int finiteCount = 0;
        for (size_t i = 0; i < lufsCurve_.size(); ++i) {
            const float v = lufsCurve_[i];
            // A silent window (NaN from the bridge, or the old sentinel) has
            // no loudness: kept as a NaN sample so the lane draws a GAP there
            // instead of joining the neighbours across it.
            if (!std::isfinite(v) || v <= -120.0f) {
                loud.samples.push_back({(float)i * lufsHopSec_, std::numeric_limits<float>::quiet_NaN()});
                continue;
            }
            loud.samples.push_back({(float)i * lufsHopSec_, v});
            lo = juce::jmin(lo, v);
            hi = juce::jmax(hi, v);
            ++finiteCount;
        }
        if (finiteCount >= 2) {
            loud.minV = std::floor(lo) - 2.0f;
            loud.maxV = juce::jmin(0.0f, std::ceil(hi) + 2.0f);
            lanes.push_back(std::move(loud));
        }
    }

    if (stereoCorrelation_.size() >= 2) {
        TimelineDataLane st;
        st.label = TRANS("Correlation");
        st.suffix = "";
        st.colour = KhensuTheme::vizCategory(2); // VIS-4: a series colour, not the minor-severity blue
        st.minV = -1.0f;
        st.maxV = 1.0f;
        st.samples = stereoCorrelation_;
        lanes.push_back(std::move(st));
    }

    heatmap_.setDataLanes(std::move(lanes));
}

void IssueHeatmapPanel::fetchLoudnessCurveIfNeeded()
{
    if (!controller_.hasActiveSession()) return;
    const auto& files = controller_.getActiveSession()->files;
    if (files.empty()) return;

    const juce::String path(files.front().path);
    if (path.isEmpty() || path == lastLoudnessPath_) return;
    lastLoudnessPath_ = path;

    fetchQuietly(*this, controller_, [path]() {
        ipc::HttpServiceBridge bridge;
        return bridge.analyzeLoudnessCurve(path.toStdString());
    }, [](IssueHeatmapPanel& self, auto&& result) {
        if (!result.is_ok()) return;
        auto v = result.value();
        self.lufsCurve_ = std::move(v.lufs_curve);
        self.lufsHopSec_ = v.hop_sec > 0.0f ? v.hop_sec : 1.0f;
        self.rebuildDataLanes();
    });
}

void IssueHeatmapPanel::fetchSpectrogramIfNeeded()
{
    if (!controller_.hasActiveSession()) return;
    const auto& files = controller_.getActiveSession()->files;
    if (files.empty()) return;

    const juce::String path(files.front().path);
    if (path.isEmpty() || path == lastSpectrogramPath_) return;
    lastSpectrogramPath_ = path;

    fetchQuietly(*this, controller_, [path]() -> std::optional<std::vector<std::vector<float>>> {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.analyzeSpectrotemporal(path.toStdString());
        if (!result.is_ok()) return std::nullopt;

        std::vector<std::vector<float>> frames;
        frames.reserve(result.value().frames.size());
        for (const auto& frame : result.value().frames)
            frames.push_back(toFloats(frame));

        return frames;
    }, [](IssueHeatmapPanel& self, std::optional<std::vector<std::vector<float>>>&& frames) {
        if (frames) self.heatmap_.setSpectrogram(std::move(*frames));
    });
}

void IssueHeatmapPanel::resized()
{
    PanelCard::resized();
    heatmap_.setBounds(bodyArea());
}

} // namespace khensu
