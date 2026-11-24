// desktop-app/Source/ui/Dashboard/SpectrogramPanel.cpp
#include "SpectrogramPanel.h"
#include <khensu/analysis/TrackTimeline.h>
#include "../shared/PanelRunState.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include "../shared/SpectrogramImage.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../../IAppController.h"
#include "../shared/FloatVector.h"
#include <cmath>
#include <algorithm>

namespace khensu {

namespace {
constexpr float kMinHz = 20.0f;
constexpr float kMaxHz = 20000.0f;
}

SpectrogramPanel::SpectrogramPanel(IAppController& c) : PanelCard(c, "Spectrogram")
{
    setExplainer(TRANS("Spectrogram"),
                 TRANS("The whole song as level over time and frequency, measured in fine detail. Brighter is louder; the "
                       "colours span the range shown on the button (0 dB is the song's loudest point). The panel at the "
                       "right is the spectrum at the cursor or playhead against the song's average (dashed). Sections run "
                       "along the top and findings mark their frequency range at the left edge. Scroll to zoom, drag to "
                       "pan, double-click for the whole song, click to play from there. 3D Waterfall shows the same data "
                       "as a terrain."),
                 "spectral_balance");
    setInterceptsMouseClicks(true, true);

    addAndMakeVisible(toggle3DButton_);
    toggle3DButton_.setButtonText(TRANS("3D Waterfall"));
    toggle3DButton_.setClickingTogglesState(true);
    toggle3DButton_.setToggleState(is3DMode_, juce::dontSendNotification);
    toggle3DButton_.onClick = [this]() {
        is3DMode_ = toggle3DButton_.getToggleState();
        toggleEqTerrainButton_.setVisible(is3DMode_);
        updateModeVisibility();
        updateZoomControl();
        repaint();
    };

    addAndMakeVisible(toggleEqTerrainButton_);
    toggleEqTerrainButton_.setButtonText(TRANS("EQ Terrain Mode"));
    toggleEqTerrainButton_.setClickingTogglesState(true);
    toggleEqTerrainButton_.setToggleState(showEqTerrain_, juce::dontSendNotification);
    toggleEqTerrainButton_.onClick = [this]() {
        showEqTerrain_ = toggleEqTerrainButton_.getToggleState();
        repaint();
    };

    toggleEqTerrainButton_.setVisible(is3DMode_);

    resetZoomButton_.onClick = [this] { resetZoom(); };
    addChildComponent(resetZoomButton_);

    addAndMakeVisible(hires_);
    hires_.onSeek = [this](double sec) {
        controller_.playback().setPlaybackPosition(sec);
        if (!controller_.playback().isPlaying()) controller_.playback().playAudio();
    };
    hires_.playheadSeconds = [this] {
        return controller_.playback().getPlaybackDuration() > 0.0 ? controller_.playback().getPlaybackPosition() : -1.0;
    };
    hires_.isPlaying = [this] { return controller_.playback().isPlaying(); };
    updateModeVisibility();

    camera3D_.pitch = 0.5f;
    camera3D_.yaw = -0.4f;
    camera3D_.zoom = 2.8f;
}

SpectrogramPanel::~SpectrogramPanel() = default;

void SpectrogramPanel::setData(const AnalysisResult& r)
{
    recommendations_ = r.recommendations;
    resonanceData_ = r.resonance;

    // Frequency-scoped recommendations become on-graph annotations (2D mode),
    // mirroring the EQ curve. Most-severe last so it washes over milder ones.
    annotations_.clear();
    for (const auto& rec : r.recommendations) {
        if (!rec.freq_range.has_value()) continue;
        const auto& fr = *rec.freq_range;
        if (!fr.is_valid() || fr.high_hz <= 0.0f || rec.severity < 20.0f) continue;
        annotations_.push_back(
            {fr.low_hz, fr.high_hz, rec.severity, juce::String(rec.what.empty() ? rec.suggested_action : rec.what)});
    }
    std::sort(annotations_.begin(), annotations_.end(),
              [](const FreqAnnotation& a, const FreqAnnotation& b) { return a.severity < b.severity; });
    std::vector<HiResSpectrogramView::FindingBand> bands;
    for (const auto& a : annotations_)
        bands.push_back({a.low_hz, a.high_hz, a.severity, a.label});
    hires_.setFindings(std::move(bands));
    hires_.setSections(r.section_analysis ? r.section_analysis->sections : std::vector<SectionData>{});
    hoveredAnnotation_ = -1;

    fetchSpectrogramIfNeeded();
    repaint();
}

// Severity -> colour, matching the Mix Notes system (red severe / amber
// moderate / blue minor).
static juce::Colour spectroSeverityColour(float severity) noexcept
{
    if (severity >= 66.0f) return KhensuTheme::accentRed;
    if (severity >= 33.0f) return KhensuTheme::accentAmber;
    return KhensuTheme::accentBlue;
}

void SpectrogramPanel::fetchSpectrogramIfNeeded()
{
    if (!controller_.hasActiveSession()) return;
    const auto& files = controller_.getActiveSession()->files;
    if (files.empty()) return;

    const juce::String path(files.front().path);
    if (path.isEmpty() || path == lastFetchedPath_) return;
    lastFetchedPath_ = path;
    frames_.clear(); // a new file: never show the previous one's spectrogram meanwhile
    imageDirty_ = true;

    struct Fetched {
        std::vector<std::vector<float>> frames;
        std::vector<float> edges;
        float duration = 0.0f;
        std::shared_ptr<const TrackTimeline> detailed;
    };
    hires_.setTimeline(nullptr);
    hires_.setStatus(TRANS("Measuring the spectrogram..."));
    // VIZ-P7: measured natively (khensu/analysis/TrackTimeline.h), so the
    // card never waits on the engine. It used to fetch once from the service
    // and, if that one request failed, kept saying "Run analysis" for good.
    fetchState_ = FetchState::Measuring;
    fetchQuietly(
        *this, controller_,
        [path]() -> std::optional<Fetched> {
            TrackTimelineOptions opt;
            opt.bands = 48;
            opt.maxFrames = 400;
            const auto tl = measureTrackTimelineFile(path.toStdString(), opt);
            if (tl.spectrogramDb.empty() || tl.bandHz.size() < 2) return std::nullopt;
            // Normalise to 0..1 over the track's own 60 dB below its 99th percentile.
            std::vector<float> all;
            for (const auto& row : tl.spectrogramDb)
                all.insert(all.end(), row.begin(), row.end());
            std::nth_element(all.begin(), all.begin() + static_cast<long>(all.size() * 99 / 100), all.end());
            const float top = all[all.size() * 99 / 100], floor = top - 60.0f;
            std::vector<std::vector<float>> frames;
            frames.reserve(tl.spectrogramDb.size());
            for (const auto& row : tl.spectrogramDb) {
                std::vector<float> f(row.size());
                for (size_t b = 0; b < row.size(); ++b)
                    f[b] = juce::jlimit(0.0f, 1.0f, (row[b] - floor) / (top - floor));
                frames.push_back(std::move(f));
            }
            // Band edges: the analyser's log grid (centres at (b + 0.5) / bands).
            std::vector<float> edges;
            const double lo = 30.0, hi = std::min(16000.0, tl.sampleRate * 0.49);
            const int bands = static_cast<int>(tl.bandHz.size());
            for (int b = 0; b <= bands; ++b)
                edges.push_back(static_cast<float>(lo * std::pow(hi / lo, static_cast<double>(b) / bands)));
            auto detailed = std::make_shared<const TrackTimeline>(
                measureTrackTimelineFile(path.toStdString(), HiResSpectrogramView::measuringOptions()));
            return Fetched{std::move(frames), std::move(edges), static_cast<float>(tl.durationSec),
                           detailed->spectrogramDb.empty() ? nullptr : detailed};
        },
        [](SpectrogramPanel& self, std::optional<Fetched>&& fetched) {
            if (!fetched) {
                self.fetchState_ = FetchState::Failed;
                self.hires_.setStatus(TRANS("The spectrogram could not be measured for this file."));
                self.lastFetchedPath_.clear(); // the next analysis update tries again
                self.repaint();
                return;
            }
            auto& [frames, edges, duration, detailed] = *fetched;
            self.hires_.setTimeline(detailed);
            self.fetchState_ = FetchState::Ready;
            self.frames_ = std::move(frames);
            self.bandEdgesHz_ = std::move(edges);
            self.durationSec_ = duration;
            self.viewStartSec_ = 0.0f;
            self.viewEndSec_ = duration;
            self.viewMinHz_ = kMinHz;
            self.viewMaxHz_ = kMaxHz;
            self.imageDirty_ = true;
            self.updateZoomControl();
            self.repaint();
        },
        nullptr, true);
}

juce::Rectangle<float> SpectrogramPanel::plotArea() const
{
    auto b = bodyArea().toFloat();
    // The frequency-axis label column and time-axis row only get drawn in 2D
    // mode (see paint()) - reserving them in 3D mode left the waterfall/EQ
    // terrain rendered into a sub-rectangle inset from the full body with
    // unused blank strips on the left/bottom, reading as "not centered".
    if (!is3DMode_) {
        b.removeFromBottom((float)kAxisH);
        b.removeFromLeft((float)kFreqLblW);
    }
    return b;
}

float SpectrogramPanel::xForTime(float sec) const
{
    auto b = plotArea();
    const float end = viewEndSec_ > viewStartSec_ ? viewEndSec_ : durationSec_;
    if (end <= viewStartSec_) return b.getX();
    return b.getX() + juce::jlimit(0.0f, 1.0f, (sec - viewStartSec_) / (end - viewStartSec_)) * b.getWidth();
}

float SpectrogramPanel::timeForX(float x) const
{
    auto b = plotArea();
    const float end = viewEndSec_ > viewStartSec_ ? viewEndSec_ : durationSec_;
    if (b.getWidth() <= 0.0f || end <= viewStartSec_) return viewStartSec_;
    return juce::jlimit(viewStartSec_, end, viewStartSec_ + (x - b.getX()) / b.getWidth() * (end - viewStartSec_));
}

float SpectrogramPanel::yForFreq(float hz) const
{
    auto b = plotArea();
    const float minHz = juce::jlimit(kMinHz, kMaxHz, viewMinHz_);
    const float maxHz = juce::jmax(minHz * 1.001f, juce::jlimit(kMinHz, kMaxHz, viewMaxHz_));
    const float t = std::log(juce::jlimit(minHz, maxHz, hz) / minHz) / std::log(maxHz / minHz);
    return b.getBottom() - t * b.getHeight();
}

float SpectrogramPanel::freqForY(float y) const
{
    auto b = plotArea();
    const float minHz = juce::jlimit(kMinHz, kMaxHz, viewMinHz_);
    const float maxHz = juce::jmax(minHz * 1.001f, juce::jlimit(kMinHz, kMaxHz, viewMaxHz_));
    if (b.getHeight() <= 0.0f) return minHz;
    const float t = juce::jlimit(0.0f, 1.0f, (b.getBottom() - y) / b.getHeight());
    return minHz * std::pow(maxHz / minHz, t);
}

std::vector<std::vector<float>> SpectrogramPanel::framesForCurrentView() const
{
    if (frames_.empty() || frames_.front().empty() || durationSec_ <= 0.0f) return frames_;

    const int frameCount = (int)frames_.size();
    const int bandCount = (int)frames_.front().size();
    const int firstFrame = juce::jlimit(0, frameCount - 1, (int)std::floor(viewStartSec_ / durationSec_ * frameCount));
    const int lastFrame = juce::jlimit(firstFrame + 1, frameCount,
                                       (int)std::ceil((viewEndSec_ > viewStartSec_ ? viewEndSec_ : durationSec_)
                                                      / durationSec_ * frameCount));

    int firstBand = 0;
    int lastBand = bandCount;
    if (bandEdgesHz_.size() >= 2) {
        const auto low = std::upper_bound(bandEdgesHz_.begin(), bandEdgesHz_.end(), viewMinHz_);
        const auto high = std::lower_bound(bandEdgesHz_.begin(), bandEdgesHz_.end(), viewMaxHz_);
        firstBand = juce::jlimit(0, bandCount - 1, (int)std::distance(bandEdgesHz_.begin(), low) - 1);
        lastBand = juce::jlimit(firstBand + 1, bandCount, (int)std::distance(bandEdgesHz_.begin(), high));
    }

    std::vector<std::vector<float>> visible;
    visible.reserve((size_t)(lastFrame - firstFrame));
    for (int frame = firstFrame; frame < lastFrame; ++frame) {
        const auto& source = frames_[(size_t)frame];
        visible.emplace_back(source.begin() + firstBand, source.begin() + lastBand);
    }
    return visible;
}

void SpectrogramPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    auto plot = plotArea();

    if (frames_.empty() && !hires_.isVisible()) {
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::bodyFont(12.0f));
        const juce::String text = fetchState_ == FetchState::Measuring ? TRANS("Measuring the spectrogram...")
                                  : fetchState_ == FetchState::Failed  ? TRANS("The spectrogram could not be measured for this file.")
                                  : controller_.hasAnalysis()          ? TRANS("Measuring the spectrogram...")
                                                                       : TRANS("Run analysis to see the spectrogram.");
        g.drawText(text, bodyArea(), juce::Justification::centred, false);
        return;
    }

    if (frames_.empty()) return;
    if (is3DMode_) {
        if (showEqTerrain_) {
            draw3DEqTerrain(g, plot);
        } else {
            draw3DWaterfall(g, plot);
        }
        drawHoverReadout(g, plot);
        return;
    }

    if (hires_.isVisible()) return; // VIZ-S1: the detailed view draws the 2D spectrogram

    // Traditional 2D Heatmap Mode
    if (imageDirty_ || image_.getWidth() != (int)plot.getWidth() || image_.getHeight() != (int)plot.getHeight()) {
        image_ = buildSpectrogramImage(framesForCurrentView(), (int)plot.getWidth(), (int)plot.getHeight());
        imageDirty_ = false;
    }
    g.drawImageAt(image_, (int)plot.getX(), (int)plot.getY());

    // --- Frequency axis (log scale, left edge) ---
    g.setFont(KhensuTheme::labelFont(8.5f));
    g.setColour(KhensuTheme::textTertiary);
    for (float hz : {20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f}) {
        const float y = yForFreq(hz);
        if (y < plot.getY() - 4.0f || y > plot.getBottom() + 4.0f) continue;
        juce::String label =
            hz >= 1000.0f ? juce::String(hz / 1000.0f, hz >= 10000.0f ? 0 : 1) + "k" : juce::String((int)hz);
        g.drawText(label, 0, (int)y - 6, kFreqLblW - 4, 12, juce::Justification::centredRight, false);
        g.setColour(KhensuTheme::textTertiary.withAlpha(0.15f));
        g.drawHorizontalLine(juce::roundToInt(y), plot.getX(), plot.getRight());
        g.setColour(KhensuTheme::textTertiary);
    }

    drawRecommendationAnnotations(g, plot);
    drawHoverReadout(g, plot);
    if (!dragSelection_.isEmpty()) {
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.12f));
        g.fillRect(dragSelection_);
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.8f));
        g.drawRect(dragSelection_, 1.0f);
    }
}

juce::Rectangle<float> SpectrogramPanel::annotationChipBounds(const FreqAnnotation& a) const
{
    auto plot = plotArea();
    const float midY =
        juce::jlimit(plot.getY() + 9.0f, plot.getBottom() - 9.0f, (yForFreq(a.low_hz) + yForFreq(a.high_hz)) * 0.5f);
    constexpr float kChip = 15.0f;
    return {plot.getX() + 6.0f, midY - kChip * 0.5f, kChip, kChip};
}

int SpectrogramPanel::hitTestAnnotation(juce::Point<float> pos) const
{
    if (is3DMode_) return -1;
    for (int i = (int)annotations_.size() - 1; i >= 0; --i)
        if (annotationChipBounds(annotations_[i]).expanded(2.0f).contains(pos)) return i;
    return -1;
}

void SpectrogramPanel::drawRecommendationAnnotations(juce::Graphics& g, const juce::Rectangle<float>& plot)
{
    if (annotations_.empty()) return;

    // Severity-washed horizontal bands across the frequency range.
    for (const auto& a : annotations_) {
        const float yTop = juce::jmax(plot.getY(), yForFreq(juce::jmax(a.low_hz, a.high_hz)));
        const float yBot = juce::jmin(plot.getBottom(), yForFreq(a.low_hz));
        const float h = juce::jmax(2.0f, yBot - yTop);
        const juce::Colour c = spectroSeverityColour(a.severity);
        g.setColour(c.withAlpha(0.10f));
        g.fillRect(plot.getX(), yTop, plot.getWidth(), h);
        g.setColour(c.withAlpha(0.35f));
        g.drawHorizontalLine(juce::roundToInt(yTop), plot.getX(), plot.getRight());
        g.drawHorizontalLine(juce::roundToInt(yBot), plot.getX(), plot.getRight());
    }

    // Warning chips (drawn on top).
    for (int i = 0; i < (int)annotations_.size(); ++i) {
        const auto ch = annotationChipBounds(annotations_[i]);
        const juce::Colour c = spectroSeverityColour(annotations_[i].severity);
        const bool hovered = (i == hoveredAnnotation_);
        g.setColour(KhensuTheme::bgSurface.withAlpha(0.92f));
        g.fillEllipse(ch);
        g.setColour(hovered ? c.brighter(0.4f) : c);
        g.drawEllipse(ch, hovered ? 2.0f : 1.5f);
        g.setColour(hovered ? c.brighter(0.4f) : c);
        g.setFont(KhensuTheme::labelFont(11.0f).boldened());
        g.drawText("!", ch.toNearestInt(), juce::Justification::centred, false);
    }

    // Hover tooltip - the plain-English "what".
    if (hoveredAnnotation_ >= 0 && hoveredAnnotation_ < (int)annotations_.size()) {
        const auto& a = annotations_[hoveredAnnotation_];
        const auto ch = annotationChipBounds(a);
        const juce::Colour c = spectroSeverityColour(a.severity);
        g.setFont(KhensuTheme::labelFont(9.5f));
        const int textW =
            juce::jmin((int)plot.getWidth() - 12,
                       juce::jmax(140, juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), a.label) + 16));
        const int textH = 30;
        int tx = (int)(ch.getRight() + 6.0f);
        if (tx + textW > (int)plot.getRight()) tx = (int)plot.getRight() - textW - 2;
        int ty = (int)juce::jlimit(plot.getY() + 2.0f, plot.getBottom() - textH - 2.0f, ch.getY());
        juce::Rectangle<int> box(tx, ty, textW, textH);
        g.setColour(KhensuTheme::bgSurface.withAlpha(0.96f));
        g.fillRoundedRectangle(box.toFloat(), 4.0f);
        g.setColour(c.withAlpha(0.6f));
        g.drawRoundedRectangle(box.toFloat(), 4.0f, 1.0f);
        g.setColour(KhensuTheme::textPrimary);
        g.drawFittedText(a.label, box.reduced(6, 3), juce::Justification::centredLeft, 2);
    }
}

void SpectrogramPanel::drawHoverReadout(juce::Graphics& g, const juce::Rectangle<float>& plot)
{
    if (hoverX_ < 0.0f || hoverY_ < 0.0f) return;
    if (!plot.contains(hoverX_, hoverY_)) return;

    g.setColour(KhensuTheme::textSecondary.withAlpha(0.5f));
    g.drawVerticalLine(juce::roundToInt(hoverX_), plot.getY(), plot.getBottom());
    g.drawHorizontalLine(juce::roundToInt(hoverY_), plot.getX(), plot.getRight());

    const float sec = timeForX(hoverX_);
    const float hz = freqForY(hoverY_);
    juce::String freqLabel = hz >= 1000.0f ? juce::String(hz / 1000.0f, 2) + " kHz" : juce::String((int)hz) + " Hz";
    juce::String readout = juce::String(sec, 2) + "s  " + freqLabel;

    g.setFont(KhensuTheme::labelFont(9.5f));
    const float textW = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), readout) + 12.0f;
    float tx = hoverX_ + 8.0f;
    float ty = hoverY_ - 20.0f;
    if (tx + textW > plot.getRight()) tx = hoverX_ - textW - 8.0f;
    if (ty < plot.getY()) ty = hoverY_ + 8.0f;

    juce::Rectangle<float> bg(tx, ty, textW, 16.0f);
    g.setColour(KhensuTheme::bgSurface.withAlpha(0.9f));
    g.fillRoundedRectangle(bg, 3.0f);
    g.setColour(KhensuTheme::textPrimary);
    g.drawText(readout, bg.toNearestInt(), juce::Justification::centred, false);
}

void SpectrogramPanel::draw3DWaterfall(juce::Graphics& g, const juce::Rectangle<float>& plot)
{
    g.saveState();
    g.reduceClipRegion(plot.toNearestInt());

    float w = plot.getWidth();
    float h = plot.getHeight();

    int totalFrames = (int)frames_.size();
    int totalBands = (int)frames_[0].size();

    // Downsample frames for smooth rendering
    int frameStep = juce::jmax(1, totalFrames / 40);
    int bandStep = juce::jmax(1, totalBands / 24);

    // Render back-to-front (Y goes from -1.0 to 1.0)
    for (int fi = totalFrames - 1; fi >= 0; fi -= frameStep) {
        float yVal = -1.0f + 2.0f * ((float)fi / (float)totalFrames);
        juce::Path p;
        bool first = true;

        for (int bi = 0; bi < totalBands; bi += bandStep) {
            float xVal = -1.0f + 2.0f * ((float)bi / (float)totalBands);
            float amp = frames_[(size_t)fi][(size_t)bi];
            float zVal = -0.5f + amp * 1.2f; // scale amplitude to height

            auto pt = camera3D_.project({xVal, yVal, zVal}, w, h);
            pt.x += plot.getX();
            pt.y += plot.getY();

            if (first) {
                p.startNewSubPath(pt.x, pt.y);
                first = false;
            } else {
                p.lineTo(pt.x, pt.y);
            }
        }

        // Draw wireframe line
        float t = (float)fi / (float)totalFrames;
        juce::Colour c = KhensuTheme::accentTeal.interpolatedWith(KhensuTheme::accentRed, t);
        g.setColour(c.withAlpha(0.4f + t * 0.5f));
        g.strokePath(p, juce::PathStrokeType(1.2f));
    }

    // Draw Resonance Beacons as glowing vertical lasers
    if (resonanceData_) {
        for (const auto& peak : resonanceData_->hotspots) {
            float logF = std::log(peak.hz / kMinHz) / std::log(kMaxHz / kMinHz);
            float xVal = -1.0f + 2.0f * logF;
            float yVal = 0.0f; // Center of time view

            auto ptBottom = camera3D_.project({xVal, yVal, -0.5f}, w, h);
            auto ptTop = camera3D_.project({xVal, yVal, 0.8f}, w, h);
            ptBottom.x += plot.getX();
            ptBottom.y += plot.getY();
            ptTop.x += plot.getX();
            ptTop.y += plot.getY();

            // Glowing line
            g.setColour(KhensuTheme::accentAmber.withAlpha(0.2f));
            g.drawLine(ptBottom.x, ptBottom.y, ptTop.x, ptTop.y, 4.0f);
            g.setColour(KhensuTheme::accentAmber);
            g.drawLine(ptBottom.x, ptBottom.y, ptTop.x, ptTop.y, 1.2f);

            // Beacon top dot
            g.setColour(khensu::KhensuTheme::textPrimary);
            g.fillEllipse(ptTop.x - 3.0f, ptTop.y - 3.0f, 6.0f, 6.0f);

            // Label
            g.setFont(KhensuTheme::labelFont(8.0f));
            g.setColour(KhensuTheme::textPrimary);
            juce::String lbl = juce::String((int)peak.hz) + " Hz";
            g.drawText(lbl, (int)ptTop.x - 40, (int)ptTop.y - 14, 80, 10, juce::Justification::centred);
        }
    }

    g.restoreState();
}

void SpectrogramPanel::draw3DEqTerrain(juce::Graphics& g, const juce::Rectangle<float>& plot)
{
    g.saveState();
    g.reduceClipRegion(plot.toNearestInt());

    float w = plot.getWidth();
    float h = plot.getHeight();

    // Draw two intersecting sheets: User EQ response vs Masking Energy
    // X: Frequency (-1..1)
    // Y: Dynamic Threshold (-1..1)
    // Z: Gain / Energy (-0.5..0.8)

    int stepsX = 30;
    int stepsY = 10;

    auto getEqHeight = [](float x, float y) -> float {
        // Bell curves simulating EQ bands
        float h1 = -0.3f * std::exp(-std::pow((x + 0.3f) / 0.2f, 2.0f)); // Low-mid cut
        float h2 = 0.4f * std::exp(-std::pow((x - 0.4f) / 0.3f, 2.0f));  // High boost
        float thresholdFactor = (y + 1.0f) * 0.5f;                       // Dynamic behavior
        return (h1 + h2) * thresholdFactor;
    };

    auto getMaskingHeight = [](float x, float y) -> float {
        // Conflicting track energy sheet
        float h = 0.5f * std::exp(-std::pow((x + 0.25f) / 0.25f, 2.0f)); // Overlaps with the low-mid cut
        float thresholdFactor = (y + 1.0f) * 0.4f;
        return h * thresholdFactor;
    };

    // Draw Masking Energy Sheet (Back to Front)
    for (int y = stepsY - 1; y >= 0; --y) {
        float yVal = -1.0f + 2.0f * ((float)y / (float)stepsY);
        juce::Path pEq, pMask;
        bool first = true;

        for (int x = 0; x <= stepsX; ++x) {
            float xVal = -1.0f + 2.0f * ((float)x / (float)stepsX);
            float eqZ = getEqHeight(xVal, yVal);
            float maskZ = getMaskingHeight(xVal, yVal);

            auto ptEq = camera3D_.project({xVal, yVal, eqZ}, w, h);
            auto ptMask = camera3D_.project({xVal, yVal, maskZ}, w, h);

            ptEq.x += plot.getX();
            ptEq.y += plot.getY();
            ptMask.x += plot.getX();
            ptMask.y += plot.getY();

            if (first) {
                pEq.startNewSubPath(ptEq.x, ptEq.y);
                pMask.startNewSubPath(ptMask.x, ptMask.y);
                first = false;
            } else {
                pEq.lineTo(ptEq.x, ptEq.y);
                pMask.lineTo(ptMask.x, ptMask.y);
            }

            // Detect collision (overlap within threshold)
            if (std::abs(eqZ - maskZ) < 0.15f && maskZ > 0.05f) {
                // Glowing collision marker
                g.setColour(KhensuTheme::accentRed.withAlpha(0.3f));
                g.fillEllipse(ptEq.x - 4.0f, ptEq.y - 4.0f, 8.0f, 8.0f);
            }
        }

        // Draw EQ Sheet trace
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.6f));
        g.strokePath(pEq, juce::PathStrokeType(1.5f));

        // Draw Masking Sheet trace
        g.setColour(KhensuTheme::accentBlue.withAlpha(0.4f));
        g.strokePath(pMask, juce::PathStrokeType(1.0f));
    }

    g.restoreState();
}

void SpectrogramPanel::updateModeVisibility()
{
    hires_.setVisible(!is3DMode_);
    resized();
    repaint();
}

void SpectrogramPanel::resized()
{
    {
        auto body = bodyArea();
        body.removeFromTop(24);
        hires_.setBounds(body);
    }
    auto r = bodyArea().removeFromTop(20);
    toggle3DButton_.setBounds(r.removeFromLeft(90));
    toggleEqTerrainButton_.setBounds(r.removeFromLeft(110).withX(toggle3DButton_.getRight() + 6));
    resetZoomButton_.setBounds(r.removeFromRight(82));
}

void SpectrogramPanel::resetZoom()
{
    viewStartSec_ = 0.0f;
    viewEndSec_ = durationSec_;
    viewMinHz_ = kMinHz;
    viewMaxHz_ = kMaxHz;
    imageDirty_ = true;
    updateZoomControl();
    repaint();
}

void SpectrogramPanel::updateZoomControl()
{
    const bool zoomed = viewStartSec_ > 0.001f || (durationSec_ > 0.0f && viewEndSec_ < durationSec_ - 0.001f)
                        || viewMinHz_ > kMinHz + 0.01f || viewMaxHz_ < kMaxHz - 0.01f;
    resetZoomButton_.setVisible(!is3DMode_ && zoomed);
}

void SpectrogramPanel::mouseMove(const juce::MouseEvent& e)
{
    const int a = hitTestAnnotation(e.position);
    const bool inPlot = plotArea().contains(e.position.toFloat());
    // Suppress the crosshair readout while pointing at an annotation chip.
    hoverX_ = (inPlot && a < 0) ? e.position.x : -1.0f;
    hoverY_ = (inPlot && a < 0) ? e.position.y : -1.0f;
    hoveredAnnotation_ = a;
    setMouseCursor(a >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void SpectrogramPanel::mouseExit(const juce::MouseEvent&)
{
    hoverX_ = -1.0f;
    hoverY_ = -1.0f;
    hoveredAnnotation_ = -1;
    repaint();
}

void SpectrogramPanel::mouseDown(const juce::MouseEvent& e)
{
    auto plot = plotArea();
    if (!plot.contains(e.position.toFloat())) return;
    if (is3DMode_) {
        controller_.playback().setPlaybackPosition(timeForX(e.position.x));
        return;
    }
    dragStart_ = e.position;
}

void SpectrogramPanel::mouseDrag(const juce::MouseEvent& e)
{
    if (dragStart_.x < 0.0f) return;
    dragSelection_ = juce::Rectangle<float>(dragStart_, e.position).getIntersection(plotArea());
    repaint();
}

void SpectrogramPanel::mouseUp(const juce::MouseEvent& e)
{
    if (dragStart_.x < 0.0f) return;
    const auto selection = dragSelection_;
    dragStart_ = {-1.0f, -1.0f};
    dragSelection_ = {};

    if (selection.getWidth() < 8.0f || selection.getHeight() < 8.0f) {
        controller_.playback().setPlaybackPosition(timeForX(e.position.x));
        repaint();
        return;
    }

    viewStartSec_ = timeForX(selection.getX());
    viewEndSec_ = timeForX(selection.getRight());
    viewMinHz_ = freqForY(selection.getBottom());
    viewMaxHz_ = freqForY(selection.getY());
    imageDirty_ = true;
    updateZoomControl();
    repaint();
}

} // namespace khensu
