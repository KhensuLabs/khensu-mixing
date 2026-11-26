// desktop-app/Source/ui/Dashboard/LoudnessCurvePanel.cpp
#include "LoudnessCurvePanel.h"
#include "../shared/PanelRunState.h"

#include <cmath>
#include <limits>
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/ServiceErrorHint.h"
#include "../../IAppController.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <algorithm>
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)
namespace chart = ui::chart;

LoudnessCurvePanel::LoudnessCurvePanel(IAppController& c) : PanelCard(c, "Loudness & Crest History")
{
    setExplainer(TRANS("Loudness & Crest History"), TRANS("Continuous short-term (3 s window, 1 s hop) K-weighted LUFS reading "
                                             "across the whole track (teal) -- a mastering-meter-style history "
                                             "graph, finer-grained than the Loudness Arc panel's per-section "
                                             "buckets. The dashed line marks the track's integrated LUFS. The "
                                             "grey line is the rolling peak/RMS crest factor over the same "
                                             "windows, own 0-24 dB scale -- where dynamics actually get squashed, "
                                             "not just the whole-track average. The three thin heatmap lanes at "
                                             "the bottom show per-window loudness (teal), squashing (grey), and "
                                             "harshness (red), so localized issues stay visible even when the "
                                             "curves overlap. The right strip is a probability-density histogram "
                                             "of the LUFS values; red ticks along the top mark inter-sample true-peak "
                                             "overshoots past -1 dBTP."));
    setAnalysisState(PanelAnalysisState::NotAnalyzed);
    chart_.owner = this;
    chart_.setTitle(TRANS("Short-term loudness over time"));
    chart_.setZoomable(true);
    chart_.setMargins(48, 14, 10, 20);
    chart_transport::attachClickToPlay(chart_, controller_.playback());
    addChildComponent(chart_);
}

LoudnessCurvePanel::~LoudnessCurvePanel() = default;

void LoudnessCurvePanel::setData(const AnalysisResult& r)
{
    integratedLufs_ = r.loudness.lufs_integrated;
    fetchCurveIfNeeded();
    rebuildChart();
    repaint();
}

void LoudnessCurvePanel::fetchCurveIfNeeded()
{
    if (!controller_.hasActiveSession()) return;
    const auto& files = controller_.getActiveSession()->files;
    if (files.empty()) return;

    // The version being listened to: the accepted render when there is one,
    // else the original. A render is measured with its parent as the
    // baseline, so the metric timeline recomputes only the windows the
    // render changed (CE-PLT-2).
    const juce::String original(files.front().path);
    const juce::String path = currentTarget();
    const juce::String baseline =
        path == original ? juce::String()
                         : RenderHistory::baselineFor(controller_.renders().renderTree(), controller_.renders().currentRenderTreeNode(),
                                                      original);
    if (path.isEmpty() || path == lastPath_ || loading_) return;
    lastPath_ = path;
    showingRender_ = path != original;
    loading_ = true;
    fetchFailed_ = false;
    setLoading(true); // shared skeleton while fetching
    setAnalysisState(PanelAnalysisState::Analyzing);

    juce::Component::SafePointer<LoudnessCurvePanel> self(this);
    controller_.runInBackground([self, &work = controller_, path, baseline]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.analyzeLoudnessCurve(path.toStdString());
        // Metric timeline (pipeline #6): the per-window hotspots ("which section
        // is harshest / most squashed"). Best-effort, folded into the same load.
        const bool useBaseline = baseline.isNotEmpty() && juce::File(baseline).existsAsFile();
        auto mt = bridge.analyzeMetricTimeline(path.toStdString(),
                                               useBaseline ? baseline.toStdString() : std::string());

        postIfCurrent(self, work, [result, mt, path](LoudnessCurvePanel& panel) {
            // A newer request owns the loading state. Do not let this older
            // response clear it or overwrite its curves.
            if (panel.lastPath_ != path) return;
            if (panel.currentTarget() != path) {
                // The accepted render moved on while this ran: release the
                // gate and measure the version that is current now.
                panel.loading_ = false;
                panel.setLoading(false);
                panel.fetchCurveIfNeeded();
                return;
            }
            panel.loading_ = false;
            panel.setLoading(false);
            if (!result.is_ok()) {
                // Surface the failure as a uniform error state instead of a
                // blank "no data" panel (empty/error-state rollout).
                panel.fetchFailed_ = true;
                panel.fetchError_ = serviceErrorLine(result.error());
                panel.setAnalysisState(PanelAnalysisState::NotAnalyzed);
                panel.rebuildChart();
                panel.repaint();
                return;
            }
            panel.fetchFailed_ = false;
            const auto& v = result.value();
            panel.lufsCurve_ = v.lufs_curve;
            panel.crestCurve_ = v.crest_curve;
            panel.hopSec_ = v.hop_sec > 0.0f ? v.hop_sec : 1.0f;
            panel.durationSec_ = (float)v.duration_sec;
            panel.tpThresholdDbtp_ = v.true_peak_threshold_dbtp;
            panel.tpOverCount_ = v.true_peak_over_count;
            panel.tpEvents_.clear();
            panel.tpEvents_.reserve(v.true_peak_events.size());
            for (const auto& e : v.true_peak_events)
                panel.tpEvents_.push_back({e.time_sec, e.dbtp});
            panel.rebuildLufsHistogram();

            // Build the compact hotspot caption (pipeline #6).
            panel.hotspotSummary_.clear();
            panel.metricTimeline_.clear();
            if (mt.is_ok()) {
                const juce::var v2 = mt.value();
                const double hop = v2[af::MetricTimelineResponse::hop_sec].isDouble() ? (double)v2[af::MetricTimelineResponse::hop_sec] : 1.0;
                auto* windows = v2[af::MetricTimelineResponse::windows].getArray();
                if (windows != nullptr) {
                    panel.metricTimeline_.reserve(windows->size());
                    // Silent windows carry null loudness/crest: keep them as
                    // NaN (a void var casts to 0, which painted silence as 0 LUFS).
                    const auto numOrNan = [](const juce::var& x) {
                        return x.isVoid() || x.isUndefined() ? std::numeric_limits<float>::quiet_NaN()
                                                             : static_cast<float>((double)x);
                    };
                    for (const auto& window : *windows) {
                        panel.metricTimeline_.push_back({
                            static_cast<float>((double)window[af::MetricTimelineWindow::start_sec]),
                            static_cast<float>((double)window[af::MetricTimelineWindow::end_sec]),
                            numOrNan(window[af::MetricTimelineWindow::loudness_lufs]),
                            numOrNan(window[af::MetricTimelineWindow::crest_db]),
                            static_cast<float>((double)window[af::MetricTimelineWindow::harshness]),
                        });
                    }
                }
                auto atTime = [&](const char* key) -> juce::String {
                    const juce::var h = v2[af::MetricTimelineResponse::hotspots][key];
                    if (!h.isObject()) return {};
                    const int idx = (int)h[af::MetricTimelineHotspot::window_index];
                    double t = (windows != nullptr && idx >= 0 && idx < windows->size())
                                   ? (double)(*windows)[idx][af::MetricTimelineWindow::start_sec]
                                   : idx * hop;
                    return juce::String((int)(t / 60)) + ":" + juce::String((int)t % 60).paddedLeft('0', 2);
                };
                juce::StringArray parts;
                const auto harsh = atTime("harshest");
                if (harsh.isNotEmpty()) parts.add("harshest " + harsh);
                const auto sq = atTime("most_squashed");
                if (sq.isNotEmpty()) parts.add("squashed " + sq);
                panel.hotspotSummary_ = parts.joinIntoString(juce::String(juce::CharPointer_UTF8("  \xc2\xb7  ")));
            }

            panel.setAnalysisState(PanelAnalysisState::Analyzed);
            panel.rebuildChart();
            panel.repaint();
        }, nullptr, [path](LoudnessCurvePanel& panel) {
            if (panel.lastPath_ != path) return; // a newer request owns the loading state
            // Superseded session: drop the data but release the loading
            // gate, or fetchCurveIfNeeded() would stay blocked for good.
            panel.loading_ = false;
            panel.setLoading(false);
        });
    });
}

juce::String LoudnessCurvePanel::currentTarget() const
{
    const juce::String rendered = controller_.currentRenderPath();
    if (rendered.isNotEmpty() && juce::File(rendered).existsAsFile()) return rendered;
    return controller_.hasActiveSession() && !controller_.getActiveSession()->files.empty()
               ? juce::String(controller_.getActiveSession()->files.front().path)
               : juce::String();
}

void LoudnessCurvePanel::onVBlank()
{
    // Follow the accepted render (apply / undo / redo / tree restore). The
    // path compare is cheap; the existence check only runs when it changed.
    // The playhead is the chart's (chart_transport::PlayheadFollower).
    if (lastPath_.isNotEmpty() && !loading_ && controller_.currentRenderPath() != renderSeen_) {
        renderSeen_ = controller_.currentRenderPath();
        if (currentTarget() != lastPath_) fetchCurveIfNeeded();
    }
}

juce::Rectangle<int> LoudnessCurvePanel::densityStrip() const
{
    return bodyArea().removeFromRight(kDensityStripW);
}

void LoudnessCurvePanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    b.removeFromRight(kDensityStripW);
    chart_.setBounds(b);
}

void LoudnessCurvePanel::rebuildLufsHistogram()
{
    lufsHistogram_.assign(kHistBins, 0.0f);
    if (lufsCurve_.empty()) return;
    for (float v : lufsCurve_) {
        if (!std::isfinite(v) || v <= kLufsMin) continue; // silent windows (NaN) and sentinels
        const int bin =
            (int)juce::jlimit(0.0f, (float)kHistBins - 1.0f, (v - kLufsMin) / (kLufsMax - kLufsMin) * (float)kHistBins);
        lufsHistogram_[(size_t)bin] += 1.0f;
    }
    const float mx = *std::max_element(lufsHistogram_.begin(), lufsHistogram_.end());
    if (mx > 0.0f)
        for (auto& c : lufsHistogram_)
            c /= mx;
}

void LoudnessCurvePanel::rebuildChart()
{
    chart_.setVisible(!lufsCurve_.empty());
    if (lufsCurve_.empty()) {
        chart_.clear();
        return;
    }
    std::vector<double> t(lufsCurve_.size()), lufs(lufsCurve_.size());
    double lo = chart::scales::kLufsHi, hi = chart::scales::kLufsLo;
    for (size_t i = 0; i < lufsCurve_.size(); ++i) {
        t[i] = static_cast<double>(i) * hopSec_;
        lufs[i] = std::isfinite(lufsCurve_[i]) ? static_cast<double>(lufsCurve_[i]) : std::nan("");
        if (std::isfinite(lufs[i]) && lufs[i] > -70.0) {
            lo = std::min(lo, lufs[i]);
            hi = std::max(hi, lufs[i]);
        }
    }
    const double duration = durationSec_ > 0.0f ? static_cast<double>(durationSec_) : (t.empty() ? 1.0 : t.back());
    chart_.x = chart::scales::time(duration);
    chart_.setXRange(0.0, duration);
    // VIS-3: the standard loudness axis (narrowed to the curve, never wider
    // than -36 .. 0 LUFS unless the curve is).
    chart_.y = chart::scales::autoFit(chart::scales::lufs(), lo, hi, 6.0, 18.0);
    chart_.y2.lo = kCrestMin;
    chart_.y2.hi = kCrestMax;
    chart_.y2.unit = "dB";

    std::vector<chart::Series> series;
    chart::Series st{TRANS("Short-term"), KhensuTheme::vizCategory(0), t, lufs, chart::Series::Kind::Line, 1.8f};
    series.push_back(st);
    if (!crestCurve_.empty()) {
        std::vector<double> ct(crestCurve_.size()), crest(crestCurve_.size());
        for (size_t i = 0; i < crestCurve_.size(); ++i) {
            ct[i] = static_cast<double>(i) * hopSec_;
            crest[i] = std::isfinite(crestCurve_[i]) ? static_cast<double>(crestCurve_[i]) : std::nan("");
        }
        chart::Series c{TRANS("Crest"), KhensuTheme::vizCategory(1), ct, crest, chart::Series::Kind::Line, 1.2f};
        c.secondary = true;
        series.push_back(c);
    }
    chart_.setSeries(std::move(series));
    std::vector<chart::RefLine> lines;
    // The integrated value comes from the original's analysis: drawn only
    // while the curve is the original's, never against a render's curve.
    if (!showingRender_ && integratedLufs_ > kLufsMin)
        lines.push_back({integratedLufs_, TRANS("Integrated") + " " + juce::String(integratedLufs_, 1) + " LUFS",
                         KhensuTheme::vizMedian, false, true});
    chart_.setRefLines(std::move(lines));

    // VIS-8: the curve in words.
    juce::String summary = TRANS("Short-term loudness over time") + ": ";
    summary << (hi >= lo ? TRANS("from") + " " + juce::String(lo, 1) + " " + TRANS("to") + " " + juce::String(hi, 1) + " LUFS"
                         : TRANS("silent"));
    if (!showingRender_ && integratedLufs_ > kLufsMin)
        summary << ", " << TRANS("integrated") << " " << juce::String(integratedLufs_, 1) << " LUFS";
    if (tpOverCount_ > 0)
        summary << "; " << tpOverCount_ << " " << TRANS("true-peak overs above") << " " << juce::String(tpThresholdDbtp_, 1)
                << " dBTP";
    if (hotspotSummary_.isNotEmpty()) summary << "; " << hotspotSummary_;
    chart_.setSummary(summary + ".");
}

void LoudnessCurvePanel::Chart::paintOver(juce::Graphics& g, juce::Rectangle<float> plot)
{
    if (owner == nullptr) return;
    const auto& p = *owner;
    // Unified metric-timeline lanes (pipeline #6): per-window loudness,
    // squashing (low crest) and harshness, along the bottom on the curve's
    // time axis. VIS-4: magnitude in the sequential palette, each lane named.
    if (!p.metricTimeline_.empty()) {
        constexpr float laneH = 4.0f, gap = 1.0f;
        const float top = plot.getBottom() - 3.0f * laneH - 2.0f * gap - 2.0f;
        const auto lane = [&](int k, const juce::String& name, auto strength) {
            const float y0 = top + static_cast<float>(k) * (laneH + gap);
            g.setColour(KhensuTheme::bgVoid.withAlpha(0.55f));
            g.fillRect(plot.getX(), y0, plot.getWidth(), laneH);
            for (const auto& w : p.metricTimeline_) {
                const float raw = strength(w);
                if (!std::isfinite(raw)) continue; // silent window: leave the lane background
                const float x0 = px(w.startSec), x1 = px(w.endSec);
                g.setColour(KhensuTheme::vizSequential(juce::jlimit(0.0f, 1.0f, raw)));
                g.fillRect(x0, y0, juce::jmax(1.0f, x1 - x0), laneH);
            }
            g.setColour(style().muted);
            g.setFont(font(0.8f));
            g.drawText(name, juce::Rectangle<float>(plot.getX() + 2.0f, y0 - 9.0f, 80.0f, 9.0f),
                       juce::Justification::centredLeft, false);
        };
        // Only the top lane is named in place; the readout names all three.
        lane(0, TRANS("Lanes: loudness / squash / harshness"),
             [](const MetricTimelineWindow& w) { return (w.loudnessLufs - kLufsMin) / (kLufsMax - kLufsMin); });
        lane(1, {}, [](const MetricTimelineWindow& w) { return (kCrestMax - w.crestDb) / (kCrestMax - kCrestMin); });
        lane(2, {}, [](const MetricTimelineWindow& w) { return w.harshness; });
    }
    // True-peak overs past the ceiling: a defect, so the severity red - with
    // the count written out (VIS-4).
    if (!p.tpEvents_.empty()) {
        const float laneTop = plot.getY() + 1.0f;
        for (const auto& e : p.tpEvents_) {
            const float over = juce::jmax(0.0f, e.y - p.tpThresholdDbtp_);
            const float frac = juce::jlimit(0.0f, 1.0f, over / 3.0f); // 3 dB over = full
            g.setColour(KhensuTheme::accentRed.withAlpha(0.45f + 0.5f * frac));
            g.fillRect(px(e.x) - 0.5f, laneTop, 1.5f, 4.0f + 8.0f * frac);
        }
        const juce::String badge = juce::String(p.tpOverCount_) + " " + TRANS("true-peak overs above") + " " +
                                   juce::String(p.tpThresholdDbtp_, 1) + " dBTP";
        g.setFont(font());
        const float bw = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), badge) + 12.0f;
        const auto r = juce::Rectangle<float>(plot.getRight() - bw - 2.0f, laneTop + 14.0f, bw, 14.0f);
        g.setColour(KhensuTheme::bgOverlay.withAlpha(0.9f));
        g.fillRoundedRectangle(r, 3.0f);
        g.setColour(KhensuTheme::accentRed);
        g.drawText(badge, r, juce::Justification::centred, false);
    }
    if (p.hotspotSummary_.isNotEmpty()) { // "which section": top-left, under the legend row
        g.setColour(style().muted);
        g.setFont(font(0.9f));
        g.drawText(p.hotspotSummary_, juce::Rectangle<float>(plot.getX() + 4.0f, plot.getY() + 2.0f, plot.getWidth() * 0.5f, 12.0f),
                   juce::Justification::centredLeft, false);
    }
}

void LoudnessCurvePanel::Chart::extendReadout(ui::chart::Readout& r, double sec)
{
    if (owner == nullptr) return;
    for (const auto& w : owner->metricTimeline_)
        if (sec >= w.startSec && sec < w.endSec) {
            if (std::isfinite(w.harshness))
                r.lines.push_back({TRANS("Harshness") + " " + juce::String(juce::roundToInt(w.harshness * 100.0f)) + " %",
                                   KhensuTheme::vizSequential(juce::jlimit(0.0f, 1.0f, w.harshness))});
            break;
        }
    for (const auto& e : owner->tpEvents_)
        if (std::abs(e.x - sec) < 0.5 * owner->hopSec_) {
            r.lines.push_back({TRANS("True peak") + " " + juce::String(e.y, 1) + " dBTP", KhensuTheme::accentRed});
            break;
        }
}

void LoudnessCurvePanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);

    if (lufsCurve_.empty()) {
        // Uniform empty/loading/error treatment via the shared PanelCard state
        // visuals. The skeleton (setLoading) covers the in-flight case, so here
        // we only paint the error and truly-empty states.
        if (fetchFailed_)
            drawEmptyState(g, "Couldn't load loudness curve", fetchError_);
        else if (!loading_)
            drawEmptyState(g, "No loudness curve yet", "Runs at intake; re-run any time.");
        return;
    }

    // --- Marginal LUFS probability-density histogram (right strip) ---
    // How much of the track sits at each loudness level, sharing the chart's
    // LUFS axis. Horizontal bars grow left->right from the plot's right edge;
    // the modal loudness reads as the widest bar.
    if (lufsHistogram_.empty()) return;
    const auto strip = densityStrip().toFloat().reduced(4.0f, 0.0f);
    const auto plot = chart_.plotArea().translated(static_cast<float>(chart_.getX()), static_cast<float>(chart_.getY()));
    if (plot.isEmpty()) return;
    const float top = plot.getY(), h = plot.getHeight();
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.35f));
    g.fillRect(strip.getX(), top, strip.getWidth(), h);
    g.setColour(KhensuTheme::vizAxis);
    g.drawVerticalLine(juce::roundToInt(strip.getX()), top, top + h);
    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.drawText(TRANS("Time at level"), juce::Rectangle<float>(strip.getX(), top - 12.0f, strip.getWidth() + 4.0f, 11.0f),
               juce::Justification::centredLeft, false);
    const float binLufs = (kLufsMax - kLufsMin) / static_cast<float>(kHistBins);
    for (int b = 0; b < kHistBins; ++b) {
        const float frac = lufsHistogram_[(size_t)b];
        if (frac <= 0.0f) continue;
        // Bin b covers [kLufsMin + b * binLufs, + binLufs], on the chart's axis.
        const float lo = kLufsMin + static_cast<float>(b) * binLufs;
        const float y0 = chart_.py(lo + binLufs) + static_cast<float>(chart_.getY());
        const float y1 = chart_.py(lo) + static_cast<float>(chart_.getY());
        if (y1 < top || y0 > top + h) continue;
        g.setColour(KhensuTheme::vizCategory(0).withAlpha(0.30f + 0.55f * frac));
        g.fillRect(strip.getX(), juce::jmax(top, y0), frac * strip.getWidth(), juce::jmax(1.0f, y1 - y0 - 0.5f));
    }
}

} // namespace khensu
