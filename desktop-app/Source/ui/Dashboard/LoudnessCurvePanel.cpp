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
    setMouseCursor(juce::MouseCursor::CrosshairCursor);
    setAnalysisState(PanelAnalysisState::NotAnalyzed);
}

LoudnessCurvePanel::~LoudnessCurvePanel() = default;

void LoudnessCurvePanel::setData(const AnalysisResult& r)
{
    integratedLufs_ = r.loudness.lufs_integrated;
    fetchCurveIfNeeded();
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
    if (lastPath_.isNotEmpty() && !loading_ && controller_.currentRenderPath() != renderSeen_) {
        renderSeen_ = controller_.currentRenderPath();
        if (currentTarget() != lastPath_) fetchCurveIfNeeded();
    }
    const float pos = static_cast<float>(controller_.playback().getPlaybackPosition());
    if (std::abs(playheadSec_ - pos) <= 0.001f) return;
    const float old = playheadSec_;
    playheadSec_ = pos;
    if (durationSec_ <= 0.0f) return;
    if (old >= 0.0f && pos >= 0.0f && juce::roundToInt(xFor(old)) == juce::roundToInt(xFor(pos))) return;
    // Only the 1 px playhead line's old and new columns (padded for AA).
    const auto body = bodyArea();
    const auto strip = [&body, this](float sec) {
        return juce::Rectangle<int>(juce::roundToInt(xFor(sec)) - 2, body.getY(), 5, body.getHeight());
    };
    if (old >= 0.0f) repaint(strip(old));
    if (pos >= 0.0f) repaint(strip(pos));
}

void LoudnessCurvePanel::resized() { PanelCard::resized(); }

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

float LoudnessCurvePanel::xFor(float sec) const
{
    const auto body = bodyArea();
    if (durationSec_ <= 0.0f) return (float)body.getX();
    // Reserve the right strip for the marginal LUFS-density histogram.
    const float w = (float)body.getWidth() - (float)kDensityStripW;
    return (float)body.getX() + juce::jlimit(0.0f, 1.0f, sec / durationSec_) * w;
}

float LoudnessCurvePanel::yForCrest(float crestDb) const
{
    const auto body = bodyArea();
    const float top = (float)body.getY() + (float)kAxisH;
    const float h = (float)body.getHeight() - (float)kAxisH;
    const float frac = juce::jlimit(0.0f, 1.0f, (crestDb - kCrestMin) / (kCrestMax - kCrestMin));
    return top + (1.0f - frac) * h;
}

float LoudnessCurvePanel::yFor(float lufs) const
{
    const auto body = bodyArea();
    const float top = (float)body.getY() + (float)kAxisH;
    const float h = (float)body.getHeight() - (float)kAxisH;
    const float frac = juce::jlimit(0.0f, 1.0f, (lufs - kLufsMin) / (kLufsMax - kLufsMin));
    return top + (1.0f - frac) * h;
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

    auto body = bodyArea();
    const int plotRight = body.getRight() - kDensityStripW;

    // Metric-timeline hotspots (pipeline #6): a compact "which section" caption in
    // the top-right of the plot — drawn over the header strip, no layout change.
    if (hotspotSummary_.isNotEmpty()) {
        g.setFont(khensu::KhensuTheme::monoFont(8.5f));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(hotspotSummary_, body.getX(), body.getY(), plotRight - body.getX(), 10,
                   juce::Justification::centredRight, false);
    }

    // Reference gridlines at -6/-14/-23 LUFS (streaming/broadcast targets).
    g.setFont(khensu::KhensuTheme::uiFont(8.0f, juce::Font::plain));
    for (float ref : {-6.0f, -14.0f, -23.0f}) {
        const float ry = yFor(ref);
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.15f));
        g.drawHorizontalLine(juce::roundToInt(ry), (float)body.getX(), (float)plotRight);
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.5f));
        g.drawText(juce::String(juce::roundToInt(ref)), body.getX() + 2, (int)ry - 10, 40, 10, juce::Justification::left, false);
    }

    // Integrated LUFS reference line.
    // The integrated value comes from the original's analysis: drawn only
    // while the curve is the original's, never against a render's curve.
    if (!showingRender_ && integratedLufs_ > kLufsMin) {
        const float iy = yFor(integratedLufs_);
        juce::Path solid;
        solid.startNewSubPath((float)body.getX(), iy);
        solid.lineTo((float)plotRight, iy);
        juce::Path dashed;
        float dashLengths[] = {4.0f, 3.0f};
        juce::PathStrokeType(1.2f).createDashedStroke(dashed, solid, dashLengths, 2);
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.6f));
        g.fillPath(dashed);
    }

    // The loudness curve itself. A silent window has no loudness (NaN from
    // the bridge): it is not drawn and the line breaks there instead of
    // diving to the floor.
    juce::Path path;
    bool penDown = false;
    for (size_t i = 0; i < lufsCurve_.size(); ++i) {
        if (!std::isfinite(lufsCurve_[i])) {
            penDown = false;
            continue;
        }
        const float t = (float)i * hopSec_;
        const float x = xFor(t);
        const float y = yFor(lufsCurve_[i]);
        if (!penDown)
            path.startNewSubPath(x, y);
        else
            path.lineTo(x, y);
        penDown = true;
    }
    g.setColour(KhensuTheme::accentTeal); // UX2-3: the one accent for the main curve
    g.strokePath(path, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved));

    // Rolling crest-factor curve (own 0-24 dB scale).
    if (!crestCurve_.empty()) {
        juce::Path crestPath;
        bool crestPenDown = false;
        for (size_t i = 0; i < crestCurve_.size(); ++i) {
            if (!std::isfinite(crestCurve_[i])) { // silent window: gap
                crestPenDown = false;
                continue;
            }
            const float t = (float)i * hopSec_;
            const float x = xFor(t);
            const float y = yForCrest(crestCurve_[i]);
            if (!crestPenDown)
                crestPath.startNewSubPath(x, y);
            else
                crestPath.lineTo(x, y);
            crestPenDown = true;
        }
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.9f));
        g.strokePath(crestPath, juce::PathStrokeType(1.3f, juce::PathStrokeType::curved));
    }

    // Unified metric-timeline heatmap (pipeline #6). These thin, aligned
    // bands retain the full per-window response rather than reducing it to
    // only the text hotspot above. Bright amber means least dynamic (lower
    // crest); bright red means more harshness. They share the curve's time
    // axis, so every cell can be read directly against the playhead.
    if (!metricTimeline_.empty()) {
        constexpr float laneHeight = 4.0f;
        constexpr float laneGap = 1.0f;
        const float lanesTop = (float)body.getBottom() - 3.0f * laneHeight - 2.0f * laneGap - 2.0f;
        const auto drawLane = [&](float y, const juce::Colour& colour, auto strength) {
            g.setColour(KhensuTheme::bgVoid.withAlpha(0.55f));
            g.fillRect((float)body.getX(), y, (float)(plotRight - body.getX()), laneHeight);
            for (const auto& window : metricTimeline_) {
                const float left = xFor(window.startSec);
                const float right = xFor(window.endSec);
                const float raw = strength(window);
                if (!std::isfinite(raw)) continue; // silent window: leave the lane background
                const float intensity = juce::jlimit(0.0f, 1.0f, raw);
                g.setColour(colour.withAlpha(0.12f + 0.82f * intensity));
                g.fillRect(left, y, juce::jmax(1.0f, right - left), laneHeight);
            }
        };
        // UX2-3: loudness and crest in the one accent; harshness is a defect
        // measure, so it keeps the severity red.
        drawLane(lanesTop, KhensuTheme::accentTeal, [](const MetricTimelineWindow& window) {
            return (window.loudnessLufs - kLufsMin) / (kLufsMax - kLufsMin);
        });
        drawLane(lanesTop + laneHeight + laneGap, KhensuTheme::textSecondary,
                 [](const MetricTimelineWindow& window) {
                     return (kCrestMax - window.crestDb) / (kCrestMax - kCrestMin);
                 });
        drawLane(lanesTop + 2.0f * (laneHeight + laneGap), KhensuTheme::accentRed,
                 [](const MetricTimelineWindow& window) { return window.harshness; });
    }

    // --- True-peak overshoot distribution (red ticks along the top) ---
    // Each inter-sample peak over the -1 dBTP ceiling gets a tick at its time;
    // hotter overshoots read taller/brighter. A badge counts them so a
    // codec-clipping risk is impossible to miss even when the ticks are dense.
    if (!tpEvents_.empty()) {
        const float laneTop = (float)body.getY() + (float)kAxisH + 1.0f;
        for (const auto& e : tpEvents_) {
            const float over = juce::jmax(0.0f, e.y - tpThresholdDbtp_);
            const float frac = juce::jlimit(0.0f, 1.0f, over / 3.0f); // 3 dB over = full
            const float tickH = 4.0f + 8.0f * frac;
            const float tx = xFor(e.x);
            g.setColour(KhensuTheme::accentRed.withAlpha(0.45f + 0.5f * frac));
            g.fillRect(tx - 0.5f, laneTop, 1.5f, tickH);
        }
        juce::String badge = juce::String(tpOverCount_) + juce::String(juce::CharPointer_UTF8(" \xe2\x9a\xa0 > ")) +
                             juce::String(tpThresholdDbtp_, 1) + " dBTP";
        g.setFont(khensu::KhensuTheme::uiFont(8.5f, juce::Font::bold));
        const int bw = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), badge) + 10;
        g.setColour(KhensuTheme::accentRed.withAlpha(0.16f));
        g.fillRoundedRectangle((float)plotRight - bw - 2.0f, laneTop, (float)bw, 13.0f, 3.0f);
        g.setColour(KhensuTheme::accentRed);
        g.drawText(badge, (int)plotRight - bw - 2, (int)laneTop, bw, 13, juce::Justification::centred, false);
    }

    // --- Marginal LUFS probability-density histogram (right strip) ---
    // How much of the track sits at each loudness level, sharing the LUFS
    // y-axis with the time curve. Horizontal bars grow left->right from the
    // plot's right edge; the modal loudness reads as the widest bar.
    if (!lufsHistogram_.empty()) {
        const float stripX = (float)plotRight + 4.0f;
        const float stripW = (float)kDensityStripW - 8.0f;
        const float top = (float)body.getY() + (float)kAxisH;
        const float h = (float)body.getHeight() - (float)kAxisH;

        g.setColour(KhensuTheme::bgVoid.withAlpha(0.35f));
        g.fillRect(stripX, top, stripW, h);
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.4f));
        g.drawVerticalLine(juce::roundToInt(stripX), top, top + h);
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.5f));
        g.setFont(khensu::KhensuTheme::uiFont(7.0f, juce::Font::plain));
        g.drawText("TIME @ LUFS", (int)stripX, (int)top - 9, (int)stripW + 4, 9, juce::Justification::centredLeft,
                   false);

        const float binH = h / (float)kHistBins;
        for (int b = 0; b < kHistBins; ++b) {
            const float frac = lufsHistogram_[(size_t)b];
            if (frac <= 0.0f) continue;
            // Bin b covers a LUFS range; higher bin index = louder = higher up.
            const float y = top + (1.0f - (float)(b + 1) / (float)kHistBins) * h;
            const float barW = frac * stripW;
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.30f + 0.55f * frac));
            g.fillRect(stripX, y, barW, juce::jmax(1.0f, binH - 0.5f));
        }
    }

    // Playhead.
    if (playheadSec_ >= 0.0f && durationSec_ > 0.0f) {
        const float px = xFor(playheadSec_);
        g.setColour(KhensuTheme::accentRed);
        g.drawVerticalLine(juce::roundToInt(px), (float)body.getY() + (float)kAxisH, (float)body.getBottom());
    }

    // Hover readout (only over the time plot, not the density strip).
    if (hoverX_ >= 0.0f && hoverX_ <= (float)plotRight && durationSec_ > 0.0f) {
        const float w = (float)body.getWidth() - (float)kDensityStripW;
        const float t = w > 0.0f ? ((hoverX_ - (float)body.getX()) / w) * durationSec_ : 0.0f;
        const size_t idx = juce::jlimit((size_t)0, lufsCurve_.size() - 1, (size_t)(t / hopSec_));
        const float lufsAtHover = lufsCurve_[idx];

        g.setColour(khensu::KhensuTheme::textPrimary.withAlpha(0.3f));
        g.drawVerticalLine(juce::roundToInt(hoverX_), (float)body.getY() + (float)kAxisH, (float)body.getBottom());

        juce::String txt = std::isfinite(lufsAtHover) ? juce::String(lufsAtHover, 1) + " LUFS" : TRANS("silent");
        g.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::plain));
        const int textW = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), txt) + 12;
        int tx = juce::jlimit(body.getX(), plotRight - textW, (int)hoverX_ - textW / 2);
        g.setColour(KhensuTheme::bgVoid.withAlpha(0.92f));
        g.fillRoundedRectangle((float)tx, (float)body.getY() + (float)kAxisH + 2.0f, (float)textW, 16.0f, 3.0f);
        g.setColour(khensu::KhensuTheme::textPrimary.withAlpha(0.9f));
        g.drawText(txt, tx, body.getY() + kAxisH + 2, textW, 16, juce::Justification::centred, false);
    }
}

void LoudnessCurvePanel::mouseMove(const juce::MouseEvent& e)
{
    hoverX_ = (float)e.x;
    repaint();
}

void LoudnessCurvePanel::mouseExit(const juce::MouseEvent&)
{
    hoverX_ = -1.0f;
    repaint();
}

} // namespace khensu
