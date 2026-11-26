// desktop-app/Source/ui/shared/SongTimelineView.cpp
#include "SongTimelineView.h"
#include "ChartStyle.h"
#include <khensu/ui/VizPalette.h>
#include <algorithm>
#include <cmath>

namespace khensu {

namespace {
constexpr float kAxisH = 18.0f;
constexpr float kLaneGap = 4.0f;
constexpr float kLufsTop = 0.0f, kLufsBottom = -42.0f;
/// Lane heights as weights of the free height; sections and findings fixed.
constexpr float kSectionsH = 20.0f, kFindingsH = 24.0f;
constexpr float kWaveWeight = 0.22f, kLoudWeight = 0.32f, kPsrWeight = 0.16f, kSpecWeight = 0.30f;

const char* laneName(int lane)
{
    switch (lane) {
    case SongTimelineView::kSections: return NEEDS_TRANS("Sections");
    case SongTimelineView::kWaveform: return NEEDS_TRANS("Waveform");
    case SongTimelineView::kLoudness: return NEEDS_TRANS("Loudness");
    case SongTimelineView::kPsr: return NEEDS_TRANS("PSR");
    case SongTimelineView::kSpectrogram: return NEEDS_TRANS("Spectrum");
    case SongTimelineView::kFindings: return NEEDS_TRANS("Findings");
    default: return "";
    }
}
} // namespace

// ---- The moment card (VIZ-2c) ------------------------------------------------------

SongTimelineView::MomentCard::MomentCard()
{
    title.setFont(KhensuTheme::bodyFont(type::secondary));
    title.setMinimumHorizontalScale(1.0f);
    title.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(title);
    addAndMakeVisible(play);
    addAndMakeVisible(open);
    play.setTooltip(TRANS("Loop this moment, with a second either side"));
    open.setTooltip(TRANS("Show this finding in the issue queue"));
}

void SongTimelineView::MomentCard::paint(juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour(KhensuTheme::shadowColour);
    g.fillRoundedRectangle(b.translated(0.0f, 2.0f).reduced(1.0f), 6.0f);
    g.setColour(KhensuTheme::bgOverlay);
    g.fillRoundedRectangle(b.reduced(1.0f), 6.0f);
    g.setColour(KhensuTheme::borderActive);
    g.drawRoundedRectangle(b.reduced(1.5f), 6.0f, 1.0f);
    title.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
}

void SongTimelineView::MomentCard::resized()
{
    auto r = getLocalBounds().reduced(10, 8);
    auto buttons = r.removeFromBottom(KhensuTheme::buttonHSmall);
    play.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2 - 3));
    buttons.removeFromLeft(6);
    open.setBounds(buttons);
    title.setBounds(r.withTrimmedBottom(4));
}

// ---- Data ------------------------------------------------------------------------

SongTimelineView::SongTimelineView()
{
    setTitle(TRANS("Song timeline"));
    setDescription(TRANS("Sections, waveform, loudness, PSR, spectrum and findings on one time axis"));
    time_.format = ui::chart::Format::Time;
    // STU-3: PSR on its own dB axis; 0-20 dB holds every master from
    // brick-walled to fully dynamic.
    psrAxis_.lo = 0.0;
    psrAxis_.hi = 20.0;
    psrAxis_.unit = "dB";
    psrAxis_.decimals = 1;
    addChildComponent(card_);
    card_.play.onClick = [this] {
        if (selected_ && *selected_ < static_cast<int>(markers_.size()) && onPlayMoment) {
            const auto& m = markers_[static_cast<size_t>(*selected_)];
            onPlayMoment(std::max(0.0, m.startSec - 1.0), std::min(fullDuration_, std::max(m.endSec, m.startSec + 0.5) + 1.0));
        }
    };
    card_.open.onClick = [this] {
        if (selected_ && *selected_ < static_cast<int>(markers_.size()) && onShowFinding)
            onShowFinding(markers_[static_cast<size_t>(*selected_)].key);
    };
    setStatus(TRANS("The timeline appears when the track is loaded."));
}

SongTimelineView::~SongTimelineView() = default;

void SongTimelineView::setTimeline(std::shared_ptr<const TrackTimeline> timeline)
{
    timeline_ = std::move(timeline);
    fullDuration_ = timeline_ ? timeline_->durationSec : 0.0;
    time_.lo = 0.0;
    time_.hi = std::max(1.0, fullDuration_);
    psr_.clear();
    squashed_.clear();
    if (timeline_) {
        psr_ = psr::series(timeline_->peakDbfs, timeline_->shortTermLufs, timeline_->hopSec);
        squashed_ = psr::squashedPassages(psr_, timeline_->hopSec);
    }
    rebuildSpectrogramImage();
    repaint();
}

void SongTimelineView::setComparison(std::shared_ptr<const TrackTimeline> timeline, const juce::String& name,
                                     juce::Colour colour)
{
    comparison_ = std::move(timeline);
    comparisonName_ = name;
    comparisonColour_ = colour;
    repaint();
}

void SongTimelineView::setSections(std::vector<SectionData> sections)
{
    sections_ = std::move(sections);
    repaint();
}

void SongTimelineView::setMarkers(std::vector<Marker> markers)
{
    markers_ = std::move(markers);
    selected_.reset();
    card_.setVisible(false);
    repaint();
}

void SongTimelineView::setLoudnessWindow(std::optional<std::pair<float, float>> lufsWindow)
{
    lufsWindow_ = lufsWindow;
    repaint();
}

void SongTimelineView::setStatus(const juce::String& text)
{
    status_ = text;
    repaint();
}

void SongTimelineView::setPlayhead(double seconds)
{
    if (std::abs(seconds - playhead_) < 1e-3) return;
    const float oldX = xForTime(playhead_), newX = xForTime(seconds);
    playhead_ = seconds;
    const int top = 0, h = getHeight();
    repaint(juce::Rectangle<int>(juce::roundToInt(oldX) - 3, top, 6, h));
    repaint(juce::Rectangle<int>(juce::roundToInt(newX) - 3, top, 6, h));
}

void SongTimelineView::rebuildSpectrogramImage()
{
    spectrogramImage_ = {};
    if (!timeline_ || timeline_->spectrogramDb.empty()) return;
    const auto& s = timeline_->spectrogramDb;
    const int frames = static_cast<int>(s.size());
    const int bands = static_cast<int>(s.front().size());
    if (bands == 0) return;
    // Scale to the track's own range: its loudest band power and 60 dB below.
    std::vector<float> all;
    all.reserve(static_cast<size_t>(frames * bands));
    for (const auto& row : s)
        all.insert(all.end(), row.begin(), row.end());
    std::nth_element(all.begin(), all.begin() + static_cast<long>(all.size() * 99 / 100), all.end());
    specTopDb_ = all[all.size() * 99 / 100];
    specFloorDb_ = specTopDb_ - 60.0f;
    spectrogramImage_ = juce::Image(juce::Image::RGB, frames, bands, false);
    juce::Image::BitmapData px(spectrogramImage_, juce::Image::BitmapData::writeOnly);
    for (int f = 0; f < frames; ++f)
        for (int b = 0; b < bands; ++b) {
            const float t = (s[static_cast<size_t>(f)][static_cast<size_t>(b)] - specFloorDb_) / (specTopDb_ - specFloorDb_);
            px.setPixelColour(f, bands - 1 - b, ui::viz::spectrogram(juce::jlimit(0.0f, 1.0f, t)));
        }
}

// ---- Layout ----------------------------------------------------------------------

void SongTimelineView::resized()
{
    card_.setVisible(false);
    selected_.reset();
}

juce::Rectangle<float> SongTimelineView::laneArea(int lane) const
{
    auto r = getLocalBounds().toFloat();
    r.removeFromLeft(static_cast<float>(kLabelW));
    r.removeFromRight(8.0f);
    r.removeFromBottom(kAxisH);
    const float free = std::max(0.0f, r.getHeight() - kSectionsH - kFindingsH - static_cast<float>(kLaneCount - 1) * kLaneGap);
    const float heights[kLaneCount] = {kSectionsH,          free * kWaveWeight, free * kLoudWeight,
                                       free * kPsrWeight,   free * kSpecWeight, kFindingsH};
    float y = r.getY();
    for (int i = 0; i < kLaneCount; ++i) {
        if (i == lane) return {r.getX(), y, r.getWidth(), heights[i]};
        y += heights[i] + kLaneGap;
    }
    return {};
}

float SongTimelineView::xForTime(double sec) const
{
    const auto r = laneArea(kWaveform);
    return r.getX() + static_cast<float>(time_.norm(sec)) * r.getWidth();
}

double SongTimelineView::timeForX(float x) const
{
    const auto r = laneArea(kWaveform);
    return r.getWidth() > 0.0f ? time_.denorm((x - r.getX()) / r.getWidth()) : 0.0;
}

float SongTimelineView::yForLufs(juce::Rectangle<float> r, float lufs) const
{
    const float t = (kLufsTop - juce::jlimit(kLufsBottom, kLufsTop, lufs)) / (kLufsTop - kLufsBottom);
    return r.getY() + t * r.getHeight();
}

juce::Colour SongTimelineView::severityColour(float severity) const
{
    if (severity >= 66.0f) return KhensuTheme::accentRed;
    if (severity >= 33.0f) return KhensuTheme::accentAmber;
    return KhensuTheme::accentBlue;
}

int SongTimelineView::markerAt(juce::Point<float> pos) const
{
    const auto lane = laneArea(kFindings);
    if (!lane.expanded(0.0f, 3.0f).contains(pos)) return -1;
    int best = -1;
    float bestD = 1e9f;
    for (size_t i = 0; i < markers_.size(); ++i) {
        const auto& m = markers_[i];
        const float x0 = xForTime(m.startSec) - 5.0f, x1 = std::max(xForTime(m.endSec), xForTime(m.startSec)) + 5.0f;
        if (pos.x < x0 || pos.x > x1) continue;
        const float d = std::abs(pos.x - 0.5f * (x0 + x1));
        if (d < bestD) {
            bestD = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

// ---- Painting --------------------------------------------------------------------

void SongTimelineView::paint(juce::Graphics& g)
{
    const auto style = desktopChartStyle();
    g.setFont(KhensuTheme::labelFont(type::caption));
    for (int lane = 0; lane < kLaneCount; ++lane) {
        const auto r = laneArea(lane);
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(juce::translate(laneName(lane)), juce::Rectangle<float>(0.0f, r.getY(), kLabelW - 8.0f, std::min(r.getHeight(), 16.0f)),
                   juce::Justification::centredRight, false);
        g.setColour(KhensuTheme::bgVoid.withAlpha(0.45f));
        g.fillRoundedRectangle(r, 4.0f);
    }
    if (!timeline_) {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::bodyFont(type::body));
        g.drawText(status_, laneArea(kLoudness), juce::Justification::centred, true);
        return;
    }
    {
        juce::Graphics::ScopedSaveState s(g);
        auto clip = laneArea(kSections).getUnion(laneArea(kFindings));
        g.reduceClipRegion(clip.toNearestInt());
        paintSections(g, laneArea(kSections));
        paintWaveform(g, laneArea(kWaveform));
        paintLoudness(g, laneArea(kLoudness));
        paintPsr(g, laneArea(kPsr));
        paintSpectrogram(g, laneArea(kSpectrogram));
        paintFindings(g, laneArea(kFindings));
    }
    paintTimeAxis(g);

    const auto top = laneArea(kSections), bottom = laneArea(kFindings);
    // The playhead through every lane.
    if (playhead_ >= time_.lo && playhead_ <= time_.hi) {
        const float x = xForTime(playhead_);
        g.setColour(KhensuTheme::accentTeal);
        g.fillRect(juce::Rectangle<float>(x - 1.0f, top.getY(), 2.0f, bottom.getBottom() - top.getY()));
    }
    // The crosshair and its readout.
    if (hoverSec_ && !card_.isVisible()) {
        const float x = xForTime(*hoverSec_);
        g.setColour(style.crosshair);
        g.drawVerticalLine(juce::roundToInt(x), top.getY(), bottom.getBottom());
        ui::chart::ChartView::paintReadout(g, readoutAt(*hoverSec_), hoverPos_, getLocalBounds().toFloat(), style);
    }
}

void SongTimelineView::paintTimeAxis(juce::Graphics& g) const
{
    const auto r = laneArea(kWaveform);
    const float y = laneArea(kFindings).getBottom() + 2.0f;
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.setColour(KhensuTheme::textTertiary);
    for (const double t : time_.ticks(std::max(2, static_cast<int>(r.getWidth() / 80.0f)))) {
        const float x = xForTime(t);
        g.drawText(time_.label(t), juce::Rectangle<float>(x - 30.0f, y, 60.0f, kAxisH - 2.0f),
                   juce::Justification::centredTop, false);
    }
}

void SongTimelineView::paintSections(juce::Graphics& g, juce::Rectangle<float> r) const
{
    std::vector<std::string> groups;
    for (const auto& s : sections_) {
        if (s.end_sec <= s.start_sec) continue;
        const std::string group = s.group.empty() ? s.role : s.group;
        auto it = std::find(groups.begin(), groups.end(), group);
        const int slot = static_cast<int>(it - groups.begin());
        if (it == groups.end()) groups.push_back(group);
        const float x0 = xForTime(s.start_sec), x1 = xForTime(s.end_sec);
        auto block = juce::Rectangle<float>(x0 + 1.0f, r.getY() + 2.0f, std::max(1.0f, x1 - x0 - 2.0f), r.getHeight() - 4.0f);
        g.setColour(KhensuTheme::vizCategory(slot).withAlpha(0.55f));
        g.fillRoundedRectangle(block, 3.0f);
        g.setColour(KhensuTheme::textPrimary);
        g.setFont(KhensuTheme::labelFont(type::caption));
        if (block.getWidth() > 34.0f)
            g.drawText(juce::String(juce::CharPointer_UTF8(s.label.c_str())), block.reduced(4.0f, 0.0f),
                       juce::Justification::centredLeft, true);
    }
    if (sections_.empty()) {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.drawText(TRANS("Sections appear after the deep analysis"), r.reduced(6.0f, 0.0f), juce::Justification::centredLeft,
                   false);
    }
}

void SongTimelineView::paintWaveform(juce::Graphics& g, juce::Rectangle<float> r) const
{
    const auto& peaks = timeline_->peakDbfs;
    if (peaks.empty()) return;
    const float mid = r.getCentreY(), half = r.getHeight() * 0.46f;
    juce::Path env;
    const int w = std::max(1, juce::roundToInt(r.getWidth()));
    std::vector<float> top(static_cast<size_t>(w), 0.0f);
    for (int px = 0; px < w; ++px) {
        const double t0 = timeForX(r.getX() + static_cast<float>(px)), t1 = timeForX(r.getX() + static_cast<float>(px + 1));
        const auto i0 = static_cast<size_t>(std::clamp(t0 / timeline_->hopSec, 0.0, static_cast<double>(peaks.size() - 1)));
        const auto i1 = static_cast<size_t>(std::clamp(t1 / timeline_->hopSec, 0.0, static_cast<double>(peaks.size() - 1)));
        float p = -240.0f;
        for (size_t i = i0; i <= std::max(i0, i1); ++i)
            p = std::max(p, peaks[i]);
        top[static_cast<size_t>(px)] = juce::jlimit(0.0f, 1.0f, std::pow(10.0f, p / 20.0f));
    }
    env.startNewSubPath(r.getX(), mid);
    for (int px = 0; px < w; ++px)
        env.lineTo(r.getX() + static_cast<float>(px), mid - top[static_cast<size_t>(px)] * half);
    for (int px = w; px-- > 0;)
        env.lineTo(r.getX() + static_cast<float>(px), mid + top[static_cast<size_t>(px)] * half);
    env.closeSubPath();
    g.setColour(KhensuTheme::textSecondary.withAlpha(0.55f));
    g.fillPath(env);
    // Where the sample peak reaches full scale, a red tick (it clips).
    g.setColour(KhensuTheme::accentRed);
    for (int px = 0; px < w; ++px)
        if (top[static_cast<size_t>(px)] >= 0.999f)
            g.fillRect(r.getX() + static_cast<float>(px), r.getY(), 1.0f, 3.0f);
}

void SongTimelineView::paintLoudness(juce::Graphics& g, juce::Rectangle<float> r) const
{
    // The genre's loudness window as a band (VIZ-1's target, in time).
    if (lufsWindow_) {
        const float y0 = yForLufs(r, lufsWindow_->second), y1 = yForLufs(r, lufsWindow_->first);
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.08f));
        g.fillRect(juce::Rectangle<float>(r.getX(), y0, r.getWidth(), std::max(1.0f, y1 - y0)));
    }
    g.setColour(KhensuTheme::vizGrid);
    g.setFont(KhensuTheme::labelFont(type::caption));
    for (float lufs = -36.0f; lufs <= -6.0f; lufs += 6.0f) {
        const float y = yForLufs(r, lufs);
        g.setColour(KhensuTheme::vizGrid);
        g.drawHorizontalLine(juce::roundToInt(y), r.getX(), r.getRight());
        g.setColour(KhensuTheme::textTertiary.withAlpha(0.8f));
        g.drawText(juce::String(juce::roundToInt(lufs)), juce::Rectangle<float>(r.getX() + 2.0f, y - 11.0f, 30.0f, 10.0f),
                   juce::Justification::centredLeft, false);
    }
    auto line = [&](const TrackTimeline& tl, const std::vector<float>& series) {
        juce::Path p;
        bool pen = false;
        const int w = std::max(1, juce::roundToInt(r.getWidth()));
        for (int px = 0; px <= w; px += 2) {
            const double t = timeForX(r.getX() + static_cast<float>(px));
            if (t < 0.0 || t > tl.durationSec) { pen = false; continue; }
            const float v = TrackTimeline::at(series, tl.hopSec, t);
            const float y = yForLufs(r, v);
            if (!pen) p.startNewSubPath(r.getX() + static_cast<float>(px), y);
            else p.lineTo(r.getX() + static_cast<float>(px), y);
            pen = true;
        }
        return p;
    };
    g.setColour(KhensuTheme::vizCategory(0).withAlpha(0.35f));
    g.strokePath(line(*timeline_, timeline_->momentaryLufs), juce::PathStrokeType(1.0f));
    if (comparison_) {
        g.setColour(comparisonColour_.withAlpha(0.9f));
        juce::Path dashed;
        const float dashes[] = {4.0f, 3.0f};
        juce::PathStrokeType(1.4f).createDashedStroke(dashed, line(*comparison_, comparison_->shortTermLufs), dashes, 2);
        g.fillPath(dashed);
    }
    g.setColour(KhensuTheme::vizCategory(0));
    g.strokePath(line(*timeline_, timeline_->shortTermLufs),
                 juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    // VIZ-4c: true-peak events where they happen.
    for (const auto& e : timeline_->truePeakEvents) {
        if (e.endSec < time_.lo || e.startSec > time_.hi) continue;
        const float x0 = xForTime(e.startSec), x1 = std::max(x0 + 2.0f, xForTime(e.endSec));
        g.setColour(KhensuTheme::accentRed.withAlpha(0.18f));
        g.fillRect(juce::Rectangle<float>(x0, r.getY(), x1 - x0, r.getHeight()));
        juce::Path tri;
        tri.addTriangle(0.5f * (x0 + x1), r.getY() + 7.0f, 0.5f * (x0 + x1) - 4.0f, r.getY() + 1.0f,
                        0.5f * (x0 + x1) + 4.0f, r.getY() + 1.0f);
        g.setColour(KhensuTheme::accentRed);
        g.fillPath(tri);
    }
}

void SongTimelineView::paintPsr(juce::Graphics& g, juce::Rectangle<float> r) const
{
    // STU-3: the peak to short-term loudness ratio on its ChartKit axis.
    auto yFor = [&](double db) {
        return r.getBottom() - static_cast<float>(psrAxis_.norm(juce::jlimit(psrAxis_.lo, psrAxis_.hi, db))) * r.getHeight();
    };
    // The squashed zone (under 6 dB) as a band, labelled so it reads without colour.
    const float yLimit = yFor(psr::kSquashedBelowDb);
    g.setColour(KhensuTheme::accentAmber.withAlpha(0.07f));
    g.fillRect(juce::Rectangle<float>(r.getX(), yLimit, r.getWidth(), r.getBottom() - yLimit));
    g.setFont(KhensuTheme::labelFont(type::caption));
    for (const double t : psrAxis_.ticks(std::max(2, static_cast<int>(r.getHeight() / 18.0f)))) {
        const float y = yFor(t);
        g.setColour(KhensuTheme::vizGrid);
        g.drawHorizontalLine(juce::roundToInt(y), r.getX(), r.getRight());
        g.setColour(KhensuTheme::textTertiary.withAlpha(0.8f));
        g.drawText(psrAxis_.label(t), juce::Rectangle<float>(r.getX() + 2.0f, y - 11.0f, 30.0f, 10.0f),
                   juce::Justification::centredLeft, false);
    }
    if (psr_.empty()) return;
    // Squashed passages: shaded, with the word where there is room.
    for (const auto& p : squashed_) {
        if (p.endSec < time_.lo || p.startSec > time_.hi) continue;
        const float x0 = xForTime(p.startSec), x1 = std::max(x0 + 2.0f, xForTime(p.endSec));
        g.setColour(KhensuTheme::accentAmber.withAlpha(0.16f));
        g.fillRect(juce::Rectangle<float>(x0, r.getY(), x1 - x0, r.getHeight()));
        if (x1 - x0 > 56.0f) {
            g.setColour(KhensuTheme::textPrimary);
            g.drawText(TRANS("Squashed"), juce::Rectangle<float>(x0 + 3.0f, r.getY() + 1.0f, x1 - x0 - 6.0f, 12.0f),
                       juce::Justification::centredLeft, true);
        }
    }
    juce::Path line;
    bool pen = false;
    const int w = std::max(1, juce::roundToInt(r.getWidth()));
    for (int px = 0; px <= w; px += 2) {
        const double t = timeForX(r.getX() + static_cast<float>(px));
        const float v = psr::at(psr_, timeline_->hopSec, t);
        if (t < 0.0 || t > timeline_->durationSec || !psr::measured(v)) {
            pen = false;
            continue;
        }
        const float x = r.getX() + static_cast<float>(px), y = yFor(v);
        if (!pen) line.startNewSubPath(x, y);
        else line.lineTo(x, y);
        pen = true;
    }
    g.setColour(KhensuTheme::vizCategory(1));
    g.strokePath(line, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void SongTimelineView::paintSpectrogram(juce::Graphics& g, juce::Rectangle<float> r)
{
    if (spectrogramImage_.isValid() && timeline_->durationSec > 0.0) {
        // The image covers 0..duration; draw the visible slice.
        const float imgW = static_cast<float>(spectrogramImage_.getWidth());
        const auto sx = static_cast<float>(time_.lo / timeline_->durationSec) * imgW;
        const auto sw = std::max(1.0f, static_cast<float>((time_.hi - time_.lo) / timeline_->durationSec) * imgW);
        g.setImageResamplingQuality(juce::Graphics::mediumResamplingQuality);
        const auto dst = r.toNearestInt();
        g.drawImage(spectrogramImage_, dst.getX(), dst.getY(), dst.getWidth(), dst.getHeight(), juce::roundToInt(sx), 0,
                    juce::roundToInt(sw), spectrogramImage_.getHeight());
    }
    // Findings with a frequency range and a time: boxes where they live.
    const auto& hz = timeline_->bandHz;
    if (hz.size() < 2) return;
    auto yForHz = [&](float f) {
        const float t = std::log(juce::jlimit(hz.front(), hz.back(), f) / hz.front()) / std::log(hz.back() / hz.front());
        return r.getBottom() - t * r.getHeight();
    };
    for (size_t i = 0; i < markers_.size(); ++i) {
        const auto& m = markers_[i];
        if (m.highHz <= 0.0f) continue;
        const float x0 = xForTime(m.startSec), x1 = std::max(x0 + 3.0f, xForTime(m.endSec));
        const float y0 = yForHz(m.highHz), y1 = yForHz(std::max(m.lowHz, hz.front()));
        const bool sel = selected_ && *selected_ == static_cast<int>(i);
        g.setColour(severityColour(m.severity).withAlpha(sel ? 1.0f : 0.8f));
        g.drawRect(juce::Rectangle<float>(x0, y0, x1 - x0, std::max(3.0f, y1 - y0)), sel ? 2.0f : 1.2f);
    }
    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::labelFont(type::caption));
    for (const float f : {100.0f, 1000.0f, 10000.0f})
        if (f > hz.front() && f < hz.back())
            g.drawText(f >= 1000.0f ? juce::String(juce::roundToInt(f / 1000.0f)) + "k" : juce::String(juce::roundToInt(f)),
                       juce::Rectangle<float>(r.getX() + 2.0f, yForHz(f) - 6.0f, 30.0f, 12.0f),
                       juce::Justification::centredLeft, false);
}

void SongTimelineView::paintFindings(juce::Graphics& g, juce::Rectangle<float> r) const
{
    if (markers_.empty()) {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.drawText(TRANS("No finding is tied to a moment"), r.reduced(6.0f, 0.0f), juce::Justification::centredLeft, false);
        return;
    }
    for (size_t i = 0; i < markers_.size(); ++i) {
        const auto& m = markers_[i];
        const float x0 = xForTime(m.startSec), x1 = std::max(x0 + 4.0f, xForTime(m.endSec));
        const bool sel = selected_ && *selected_ == static_cast<int>(i);
        const auto c = severityColour(m.severity);
        auto bar = juce::Rectangle<float>(x0, r.getY() + 5.0f, x1 - x0, r.getHeight() - 10.0f);
        g.setColour(c.withAlpha(sel ? 0.95f : 0.7f));
        g.fillRoundedRectangle(bar, 2.0f);
        if (sel) {
            g.setColour(KhensuTheme::textPrimary);
            g.drawRoundedRectangle(bar.expanded(1.5f), 3.0f, 1.0f);
        }
    }
}

ui::chart::Readout SongTimelineView::readoutAt(double sec) const
{
    ui::chart::Readout r;
    ui::chart::Axis t = time_;
    r.title = t.label(sec, true);
    for (const auto& s : sections_)
        if (sec >= s.start_sec && sec < s.end_sec) {
            r.lines.push_back({juce::String(juce::CharPointer_UTF8(s.label.c_str())), KhensuTheme::textSecondary});
            break;
        }
    if (timeline_) {
        r.lines.push_back({TRANS("Short-term") + " " + juce::String(TrackTimeline::at(timeline_->shortTermLufs, timeline_->hopSec, sec), 1) + " LUFS",
                           KhensuTheme::vizCategory(0)});
        r.lines.push_back({TRANS("Momentary") + " " + juce::String(TrackTimeline::at(timeline_->momentaryLufs, timeline_->hopSec, sec), 1) + " LUFS",
                           KhensuTheme::vizCategory(0).withAlpha(0.5f)});
        r.lines.push_back({TRANS("Peak") + " " + juce::String(TrackTimeline::at(timeline_->peakDbfs, timeline_->hopSec, sec, -240.0f), 1) + " dBFS",
                           KhensuTheme::textSecondary});
        if (const float v = psr::at(psr_, timeline_->hopSec, sec); psr::measured(v))
            r.lines.push_back({TRANS("PSR") + " " + psrAxis_.label(v, true)
                                   + (v < psr::kSquashedBelowDb ? " - " + TRANS("squashed") : juce::String()),
                               KhensuTheme::vizCategory(1)});
        for (const auto& e : timeline_->truePeakEvents)
            if (sec >= e.startSec - 0.05 && sec <= e.endSec + 0.05) {
                r.lines.push_back({TRANS("True peak") + " " + juce::String(e.maxDbtp, 1) + " dBTP", KhensuTheme::accentRed});
                break;
            }
    }
    if (comparison_)
        r.lines.push_back({comparisonName_ + " " + juce::String(TrackTimeline::at(comparison_->shortTermLufs, comparison_->hopSec, sec), 1) + " LUFS",
                           comparisonColour_});
    for (const auto& m : markers_)
        if (sec >= m.startSec - 0.2 && sec <= std::max(m.endSec, m.startSec) + 0.2) {
            r.lines.push_back({m.title.upToFirstOccurrenceOf(".", false, false).substring(0, 60), severityColour(m.severity)});
            break;
        }
    return r;
}

// ---- Interaction -----------------------------------------------------------------

void SongTimelineView::showMomentCard(int marker)
{
    if (marker < 0 || marker >= static_cast<int>(markers_.size())) {
        selected_.reset();
        card_.setVisible(false);
        repaint();
        return;
    }
    selected_ = marker;
    const auto& m = markers_[static_cast<size_t>(marker)];
    ui::chart::Axis t = time_;
    card_.title.setText(t.label(m.startSec, true) + "  " + m.title, juce::dontSendNotification);
    const int w = 300, h = 96;
    const auto lane = laneArea(kFindings);
    const int x = juce::jlimit(0, std::max(0, getWidth() - w), juce::roundToInt(xForTime(m.startSec)) - w / 2);
    card_.setBounds(x, juce::roundToInt(lane.getY()) - h - 4, w, h);
    card_.setVisible(true);
    card_.toFront(false);
    repaint();
}

void SongTimelineView::mouseMove(const juce::MouseEvent& e)
{
    const auto area = laneArea(kSections).getUnion(laneArea(kFindings));
    if (timeline_ && area.contains(e.position)) {
        hoverSec_ = timeForX(e.position.x);
        hoverPos_ = e.position;
    } else {
        hoverSec_.reset();
    }
    setMouseCursor(markerAt(e.position) >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void SongTimelineView::mouseExit(const juce::MouseEvent&)
{
    hoverSec_.reset();
    repaint();
}

void SongTimelineView::mouseDown(const juce::MouseEvent& e)
{
    downPos_ = e.position;
    dragLo_ = time_.lo;
    dragHi_ = time_.hi;
    dragged_ = false;
}

void SongTimelineView::mouseDrag(const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() > 3) dragged_ = true;
    if (!dragged_ || fullDuration_ <= 0.0) return;
    const auto r = laneArea(kWaveform);
    const double span = dragHi_ - dragLo_;
    if (span >= fullDuration_ - 1e-6) return; // not zoomed: nothing to pan
    const double dt = -(e.position.x - downPos_.x) / std::max(1.0f, r.getWidth()) * span;
    const double lo = std::clamp(dragLo_ + dt, 0.0, fullDuration_ - span);
    time_.lo = lo;
    time_.hi = lo + span;
    hoverSec_.reset();
    repaint();
}

void SongTimelineView::mouseUp(const juce::MouseEvent& e)
{
    if (dragged_ || !timeline_) return;
    if (const int m = markerAt(e.position); m >= 0) {
        showMomentCard(m);
        return;
    }
    if (card_.isVisible()) showMomentCard(-1);
    const auto area = laneArea(kSections).getUnion(laneArea(kSpectrogram));
    if (area.contains(e.position) && onSeek) onSeek(std::clamp(timeForX(e.position.x), 0.0, fullDuration_));
}

void SongTimelineView::mouseDoubleClick(const juce::MouseEvent&)
{
    time_.lo = 0.0;
    time_.hi = std::max(1.0, fullDuration_);
    repaint();
}

void SongTimelineView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (fullDuration_ <= 0.0 || w.deltaY == 0.0f) {
        Component::mouseWheelMove(e, w);
        return;
    }
    const auto r = laneArea(kWaveform);
    const double t = std::clamp(static_cast<double>((e.position.x - r.getX()) / std::max(1.0f, r.getWidth())), 0.0, 1.0);
    const double centre = time_.lo + t * (time_.hi - time_.lo);
    const double span = std::clamp((time_.hi - time_.lo) * (w.deltaY > 0 ? 0.8 : 1.25), std::min(5.0, fullDuration_), fullDuration_);
    double lo = centre - t * span;
    lo = std::clamp(lo, 0.0, fullDuration_ - span);
    time_.lo = lo;
    time_.hi = lo + span;
    if (card_.isVisible()) showMomentCard(-1);
    repaint();
}

} // namespace khensu
