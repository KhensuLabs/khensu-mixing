// desktop-app/Source/ui/shared/HiResSpectrogramView.cpp
#include "HiResSpectrogramView.h"
#include "KhensuTheme.h"
#include <khensu/ui/VizPalette.h>
#include <algorithm>
#include <cmath>

namespace khensu {

namespace {

juce::String clockText(double sec, bool tenths)
{
    sec = std::max(0.0, sec);
    const int m = static_cast<int>(sec / 60.0);
    const double s = sec - m * 60.0;
    return juce::String(m) + ":"
           + (tenths ? juce::String(s, 1).paddedLeft('0', 4) : juce::String(static_cast<int>(s)).paddedLeft('0', 2));
}

juce::String hzText(float hz)
{
    if (hz >= 1000.0f) return juce::String(hz / 1000.0f, hz >= 10000.0f ? 1 : 2) + " kHz";
    return juce::String(juce::roundToInt(hz)) + " Hz";
}

juce::Colour severityColour(float severity)
{
    if (severity >= 66.0f) return KhensuTheme::accentRed;
    if (severity >= 33.0f) return KhensuTheme::accentAmber;
    return KhensuTheme::accentBlue;
}

} // namespace

HiResSpectrogramView::HiResSpectrogramView()
{
    for (size_t i = 0; i < lut_.size(); ++i)
        lut_[i] = ui::viz::spectrogram(static_cast<float>(i) / 255.0f).getPixelARGB();
    setTitle(TRANS("Detailed spectrogram"));
    setTooltip(TRANS("Level over time and frequency. Scroll to zoom, drag to pan, double-click for the whole song, "
                     "click to play from there."));
    addAndMakeVisible(rangeButton_);
    rangeButton_.setTitle(TRANS("Colour range"));
    rangeButton_.setTooltip(TRANS("How many dB below the loudest point the colours reach. Smaller shows more contrast "
                                  "in the loud parts; larger shows quiet detail."));
    rangeButton_.onClick = [this] {
        rangeDb_ = rangeDb_ <= 60.0f ? 84.0f : rangeDb_ <= 84.0f ? 108.0f : 60.0f;
        updateRangeButton();
        viewDirty_ = true;
        repaint();
    };
    updateRangeButton();
    vblank_ = std::make_unique<juce::VBlankAttachment>(this, [this] { onVBlank(); });
}

HiResSpectrogramView::~HiResSpectrogramView() = default;

TrackTimelineOptions HiResSpectrogramView::measuringOptions()
{
    TrackTimelineOptions o;
    o.bands = 256;
    o.maxFrames = 3000;
    o.fftOrder = 12;
    o.fineLowEnd = true;
    return o;
}

void HiResSpectrogramView::updateRangeButton()
{
    rangeButton_.setButtonText(juce::String(juce::roundToInt(rangeDb_)) + " dB");
}

void HiResSpectrogramView::setTimeline(std::shared_ptr<const TrackTimeline> timeline)
{
    if (timeline == timeline_) return;
    timeline_ = std::move(timeline);
    meanDb_.clear();
    if (timeline_ != nullptr && !timeline_->spectrogramDb.empty()) {
        const auto& sg = timeline_->spectrogramDb;
        const size_t bands = sg.front().size();
        std::vector<double> sum(bands, 0.0);
        std::vector<float> all;
        all.reserve(sg.size() * bands / 4 + 1);
        for (size_t f = 0; f < sg.size(); ++f)
            for (size_t b = 0; b < bands; ++b) {
                sum[b] += std::pow(10.0, sg[f][b] / 10.0);
                if ((f + b) % 4 == 0) all.push_back(sg[f][b]);
            }
        meanDb_.resize(bands);
        for (size_t b = 0; b < bands; ++b)
            meanDb_[b] = static_cast<float>(10.0 * std::log10(sum[b] / static_cast<double>(sg.size()) + 1e-12));
        const auto k = static_cast<size_t>(static_cast<double>(all.size() - 1) * 0.995);
        std::nth_element(all.begin(), all.begin() + static_cast<long>(k), all.end());
        topDb_ = all[k];
        fHi_ = std::min(16000.0, timeline_->sampleRate * 0.49);
        viewStart_ = targetStart_ = 0.0;
        viewEnd_ = targetEnd_ = timeline_->durationSec;
    }
    viewDirty_ = true;
    repaint();
}

void HiResSpectrogramView::setSections(std::vector<SectionData> sections)
{
    sections_ = std::move(sections);
    repaint();
}

void HiResSpectrogramView::setFindings(std::vector<FindingBand> findings)
{
    findings_ = std::move(findings);
    repaint();
}

void HiResSpectrogramView::setStatus(const juce::String& text)
{
    status_ = text;
    repaint();
}

juce::Rectangle<float> HiResSpectrogramView::plotArea() const
{
    auto r = getLocalBounds().toFloat();
    r.removeFromTop(kSectionH + 2.0f);
    r.removeFromBottom(kTimeAxisH);
    r.removeFromLeft(kFreqAxisW);
    r.removeFromRight(kSliceW + kLegendW + 8.0f);
    return r.getWidth() > 4.0f && r.getHeight() > 4.0f ? r : juce::Rectangle<float>();
}

double HiResSpectrogramView::timeAtX(float x) const
{
    const auto p = plotArea();
    if (p.isEmpty()) return 0.0;
    return viewStart_ + (static_cast<double>(x) - p.getX()) / p.getWidth() * (viewEnd_ - viewStart_);
}

float HiResSpectrogramView::xAtTime(double sec) const
{
    const auto p = plotArea();
    if (p.isEmpty() || viewEnd_ <= viewStart_) return p.getX();
    return p.getX() + static_cast<float>((sec - viewStart_) / (viewEnd_ - viewStart_)) * p.getWidth();
}

float HiResSpectrogramView::hzAtY(float y) const
{
    const auto p = plotArea();
    if (p.isEmpty()) return static_cast<float>(fLo_);
    const double frac = juce::jlimit(0.0, 1.0, (static_cast<double>(p.getBottom()) - y) / p.getHeight());
    return static_cast<float>(fLo_ * std::pow(fHi_ / fLo_, frac));
}

float HiResSpectrogramView::yAtHz(float hz) const
{
    const auto p = plotArea();
    const double frac = std::log(std::max(1.0, static_cast<double>(hz)) / fLo_) / std::log(fHi_ / fLo_);
    return p.getBottom() - static_cast<float>(frac) * p.getHeight();
}

juce::String HiResSpectrogramView::noteName(float hz)
{
    if (!(hz > 0.0f)) return {};
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int midi = juce::roundToInt(69.0 + 12.0 * std::log2(hz / 440.0));
    if (midi < 0 || midi > 127) return {};
    return juce::String(names[midi % 12]) + juce::String(midi / 12 - 1);
}

int HiResSpectrogramView::frameAt(double sec) const
{
    if (timeline_ == nullptr || timeline_->spectrogramDb.empty() || timeline_->frameSec <= 0.0) return -1;
    const auto n = static_cast<int>(timeline_->spectrogramDb.size());
    return juce::jlimit(0, n - 1, static_cast<int>(std::lround(sec / timeline_->frameSec)));
}

void HiResSpectrogramView::resized()
{
    const auto p = plotArea();
    rangeButton_.setBounds(juce::Rectangle<int>(static_cast<int>(p.getRight()) + 8, 0,
                                                static_cast<int>(kSliceW + kLegendW), static_cast<int>(kSectionH))
                               .reduced(0, 0));
    viewDirty_ = true;
}

void HiResSpectrogramView::setTargetView(double start, double end, bool animate)
{
    if (timeline_ == nullptr) return;
    const double dur = timeline_->durationSec;
    const double span = juce::jlimit(std::min(2.0, dur), dur, end - start);
    start = juce::jlimit(0.0, std::max(0.0, dur - span), start);
    targetStart_ = start;
    targetEnd_ = start + span;
    if (!animate) {
        viewStart_ = targetStart_;
        viewEnd_ = targetEnd_;
        viewDirty_ = true;
        repaint();
    }
}

void HiResSpectrogramView::onVBlank()
{
    bool changed = false;
    // Ease the view toward its target (scroll zoom glides rather than jumps).
    if (std::abs(targetStart_ - viewStart_) > 1e-4 || std::abs(targetEnd_ - viewEnd_) > 1e-4) {
        const double k = 0.28;
        viewStart_ += (targetStart_ - viewStart_) * k;
        viewEnd_ += (targetEnd_ - viewEnd_) * k;
        if (std::abs(targetStart_ - viewStart_) < 1e-3 && std::abs(targetEnd_ - viewEnd_) < 1e-3) {
            viewStart_ = targetStart_;
            viewEnd_ = targetEnd_;
        }
        viewDirty_ = changed = true;
    }
    if (playheadSeconds) {
        const double ph = playheadSeconds();
        if (std::abs(ph - playhead_) > 1e-3) {
            playhead_ = ph;
            changed = true;
            // Zoomed in and playing: page along with the playhead.
            const bool playing = isPlaying && isPlaying();
            if (playing && timeline_ != nullptr && ph > viewEnd_ && !dragging_) {
                const double span = targetEnd_ - targetStart_;
                setTargetView(ph - span * 0.1, ph + span * 0.9, true);
            }
        }
    }
    if (changed) repaint();
}

void HiResSpectrogramView::renderViewImage()
{
    const auto p = plotArea();
    const int w = std::max(1, static_cast<int>(p.getWidth())), h = std::max(1, static_cast<int>(p.getHeight()));
    if (!viewImage_.isValid() || viewImage_.getWidth() != w || viewImage_.getHeight() != h)
        viewImage_ = juce::Image(juce::Image::ARGB, w, h, false);
    viewDirty_ = false;
    if (timeline_ == nullptr || timeline_->spectrogramDb.empty()) return;

    const auto& sg = timeline_->spectrogramDb;
    const int frames = static_cast<int>(sg.size());
    const int bands = static_cast<int>(sg.front().size());
    const double frameSec = std::max(1e-6, timeline_->frameSec);
    const float floorDb = topDb_ - rangeDb_;
    const float scale = 255.0f / rangeDb_;
    std::vector<float> column(static_cast<size_t>(bands));

    juce::Image::BitmapData bd(viewImage_, juce::Image::BitmapData::writeOnly);
    for (int x = 0; x < w; ++x) {
        const double t0 = viewStart_ + (viewEnd_ - viewStart_) * x / w;
        const double t1 = viewStart_ + (viewEnd_ - viewStart_) * (x + 1) / w;
        const double f0 = t0 / frameSec, f1 = t1 / frameSec;
        if (f1 - f0 >= 1.0) {
            // Zoomed out: average the frames under this pixel (in power).
            const int a = juce::jlimit(0, frames - 1, static_cast<int>(f0));
            const int b = juce::jlimit(a, frames - 1, static_cast<int>(f1));
            for (int band = 0; band < bands; ++band) {
                double s = 0.0, peak = -300.0;
                for (int f = a; f <= b; ++f) {
                    const float v = sg[static_cast<size_t>(f)][static_cast<size_t>(band)];
                    s += std::pow(10.0, v / 10.0);
                    peak = std::max(peak, static_cast<double>(v));
                }
                // Lean toward the peak so short transients survive the average.
                const double mean = 10.0 * std::log10(s / (b - a + 1) + 1e-12);
                column[static_cast<size_t>(band)] = static_cast<float>(0.6 * mean + 0.4 * peak);
            }
        } else {
            // Zoomed in: interpolate between the two nearest frames.
            const double fc = juce::jlimit(0.0, static_cast<double>(frames - 1), 0.5 * (f0 + f1));
            const int a = static_cast<int>(fc), b = std::min(frames - 1, a + 1);
            const float fr = static_cast<float>(fc - a);
            for (int band = 0; band < bands; ++band) {
                const float va = sg[static_cast<size_t>(a)][static_cast<size_t>(band)];
                const float vb = sg[static_cast<size_t>(b)][static_cast<size_t>(band)];
                column[static_cast<size_t>(band)] = va + (vb - va) * fr;
            }
        }
        for (int y = 0; y < h; ++y) {
            // Row y covers a fractional band position; interpolate between bands.
            const double bp = (static_cast<double>(h - y) - 0.5) / h * bands - 0.5;
            const int b0 = juce::jlimit(0, bands - 1, static_cast<int>(std::floor(bp)));
            const int b1 = std::min(bands - 1, b0 + 1);
            const float fr = static_cast<float>(juce::jlimit(0.0, 1.0, bp - b0));
            const float db = column[static_cast<size_t>(b0)] + (column[static_cast<size_t>(b1)] - column[static_cast<size_t>(b0)]) * fr;
            const int idx = juce::jlimit(0, 255, static_cast<int>((db - floorDb) * scale));
            auto* px = reinterpret_cast<juce::PixelARGB*>(bd.getPixelPointer(x, y));
            *px = lut_[static_cast<size_t>(idx)];
        }
    }
}

void HiResSpectrogramView::paint(juce::Graphics& g)
{
    const auto plot = plotArea();
    if (timeline_ == nullptr || timeline_->spectrogramDb.empty() || plot.isEmpty()) {
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::bodyFont(12.0f));
        g.drawText(status_.isNotEmpty() ? status_ : TRANS("Measuring the spectrogram..."), getLocalBounds(),
                   juce::Justification::centred, false);
        return;
    }
    if (viewDirty_) renderViewImage();

    {
        juce::Graphics::ScopedSaveState s(g);
        juce::Path clip;
        clip.addRoundedRectangle(plot, 4.0f);
        g.reduceClipRegion(clip);
        g.drawImageAt(viewImage_, static_cast<int>(plot.getX()), static_cast<int>(plot.getY()));
        // A soft inner vignette gives the plot depth without hiding data.
        juce::ColourGradient top(juce::Colours::black.withAlpha(0.28f), plot.getX(), plot.getY(), // theme-exempt(drawn over the always-dark spectrogram map)
                                 juce::Colours::transparentBlack, plot.getX(), plot.getY() + 14.0f, false);
        g.setGradientFill(top);
        g.fillRect(plot.withHeight(14.0f));
        drawFindings(g, plot);
        drawCursor(g, plot);
    }
    g.setColour(KhensuTheme::borderSubtle);
    g.drawRoundedRectangle(plot.expanded(0.5f), 4.0f, 1.0f);

    drawSections(g, plot);
    drawAxes(g, plot);
    drawSlice(g, plot);
    drawLegend(g, plot);
}

void HiResSpectrogramView::drawAxes(juce::Graphics& g, juce::Rectangle<float> plot)
{
    g.setFont(KhensuTheme::labelFont(9.5f));
    // Frequency: decade-ish ticks with faint grid lines across the plot.
    for (float hz : {50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f}) {
        if (hz < fLo_ || hz > fHi_) continue;
        const float y = yAtHz(hz);
        g.setColour(juce::Colours::white.withAlpha(0.07f)); // theme-exempt(drawn over the always-dark spectrogram map)
        g.drawHorizontalLine(juce::roundToInt(y), plot.getX(), plot.getRight());
        g.setColour(KhensuTheme::textTertiary);
        const juce::String label = hz >= 1000.0f ? juce::String(juce::roundToInt(hz / 1000.0f)) + "k"
                                                 : juce::String(juce::roundToInt(hz));
        g.drawText(label, juce::Rectangle<float>(0.0f, y - 6.0f, kFreqAxisW - 6.0f, 12.0f),
                   juce::Justification::centredRight, false);
    }
    // Time: a step that keeps labels at least ~70 px apart.
    const double span = viewEnd_ - viewStart_;
    double step = 1.0;
    for (double s : {0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0}) {
        step = s;
        if (span / s * 70.0 <= plot.getWidth()) break;
    }
    g.setColour(KhensuTheme::textTertiary);
    for (double t = std::ceil(viewStart_ / step) * step; t <= viewEnd_ + 1e-9; t += step) {
        const float x = xAtTime(t);
        g.drawVerticalLine(juce::roundToInt(x), plot.getBottom(), plot.getBottom() + 3.0f);
        g.drawText(clockText(t, step < 1.0), juce::Rectangle<float>(x - 30.0f, plot.getBottom() + 2.0f, 60.0f, 13.0f),
                   juce::Justification::centred, false);
    }
}

void HiResSpectrogramView::drawSections(juce::Graphics& g, juce::Rectangle<float> plot)
{
    const auto strip = juce::Rectangle<float>(plot.getX(), 0.0f, plot.getWidth(), kSectionH);
    if (sections_.empty()) return;
    juce::StringArray groups;
    g.setFont(KhensuTheme::labelFont(9.5f));
    for (const auto& s : sections_) {
        const float x0 = std::max(strip.getX(), xAtTime(s.start_sec));
        const float x1 = std::min(strip.getRight(), xAtTime(s.end_sec));
        if (x1 - x0 < 1.0f) continue;
        const juce::String group(s.group.empty() ? s.label : s.group);
        if (!groups.contains(group)) groups.add(group);
        const auto colour = ui::viz::categorical(groups.indexOf(group));
        const auto r = juce::Rectangle<float>(x0, strip.getY() + 2.0f, x1 - x0, strip.getHeight() - 3.0f).reduced(1.0f, 0.0f);
        g.setColour(colour.withAlpha(0.32f));
        g.fillRoundedRectangle(r, 3.0f);
        g.setColour(colour.withAlpha(0.9f));
        g.fillRect(r.withHeight(2.0f));
        if (r.getWidth() > 34.0f) {
            g.setColour(KhensuTheme::textPrimary);
            g.drawText(juce::String(juce::CharPointer_UTF8(s.label.c_str())), r.reduced(4.0f, 0.0f).translated(0.0f, 1.0f),
                       juce::Justification::centredLeft, true);
        }
    }
}

void HiResSpectrogramView::drawFindings(juce::Graphics& g, juce::Rectangle<float> plot)
{
    for (const auto& f : findings_) {
        const float y0 = std::max(plot.getY(), yAtHz(f.highHz)), y1 = std::min(plot.getBottom(), yAtHz(f.lowHz));
        if (y1 - y0 < 1.0f) continue;
        const auto c = severityColour(f.severity);
        g.setColour(c.withAlpha(0.9f));
        g.fillRoundedRectangle(plot.getX() + 2.0f, y0, 3.0f, y1 - y0, 1.5f);
        const bool hovered = hover_.x >= plot.getX() && hover_.x <= plot.getX() + 14.0f && hover_.y >= y0 && hover_.y <= y1;
        if (hovered) {
            g.setColour(c.withAlpha(0.10f));
            g.fillRect(juce::Rectangle<float>(plot.getX(), y0, plot.getWidth(), y1 - y0));
        }
    }
}

void HiResSpectrogramView::drawCursor(juce::Graphics& g, juce::Rectangle<float> plot)
{
    // The playhead: a bright line with a soft glow.
    if (playhead_ >= viewStart_ && playhead_ <= viewEnd_) {
        const float x = xAtTime(playhead_);
        g.setColour(juce::Colours::white.withAlpha(0.18f)); // theme-exempt(drawn over the always-dark spectrogram map)
        g.fillRect(x - 2.0f, plot.getY(), 5.0f, plot.getHeight());
        g.setColour(juce::Colours::white.withAlpha(0.95f)); // theme-exempt(drawn over the always-dark spectrogram map)
        g.fillRect(x - 0.5f, plot.getY(), 1.5f, plot.getHeight());
    }
    if (dragging_ || !plot.contains(hover_)) return;

    // The crosshair and its readout chip.
    g.setColour(juce::Colours::white.withAlpha(0.35f)); // theme-exempt(drawn over the always-dark spectrogram map)
    const float dashes[] = {3.0f, 3.0f};
    g.drawDashedLine(juce::Line<float>(hover_.x, plot.getY(), hover_.x, plot.getBottom()), dashes, 2, 1.0f);
    g.drawDashedLine(juce::Line<float>(plot.getX(), hover_.y, plot.getRight(), hover_.y), dashes, 2, 1.0f);

    const double t = timeAtX(hover_.x);
    const float hz = hzAtY(hover_.y);
    const int f = frameAt(t);
    const auto& sg = timeline_->spectrogramDb;
    const int bands = static_cast<int>(sg.front().size());
    const double frac = std::log(hz / fLo_) / std::log(fHi_ / fLo_);
    const int band = juce::jlimit(0, bands - 1, static_cast<int>(frac * bands));
    const float db = f >= 0 ? sg[static_cast<size_t>(f)][static_cast<size_t>(band)] : -120.0f;
    juce::String text = clockText(t, true) + "   " + hzText(hz);
    if (const auto note = noteName(hz); note.isNotEmpty()) text << "  " << note;
    text << "   " << juce::String(db - topDb_, 1) << " dB";

    const auto font = KhensuTheme::monoFont(11.0f);
    g.setFont(font);
    const float tw = juce::GlyphArrangement::getStringWidth(font, text) + 16.0f;
    auto chip = juce::Rectangle<float>(hover_.x + 12.0f, hover_.y - 26.0f, tw, 20.0f);
    if (chip.getRight() > plot.getRight() - 2.0f) chip.setX(hover_.x - 12.0f - tw);
    if (chip.getY() < plot.getY() + 2.0f) chip.setY(hover_.y + 8.0f);
    g.setColour(KhensuTheme::bgOverlay.withAlpha(0.94f));
    g.fillRoundedRectangle(chip, 5.0f);
    g.setColour(KhensuTheme::borderSubtle);
    g.drawRoundedRectangle(chip, 5.0f, 1.0f);
    g.setColour(KhensuTheme::textPrimary);
    g.drawText(text, chip, juce::Justification::centred, false);
}

void HiResSpectrogramView::drawSlice(juce::Graphics& g, juce::Rectangle<float> plot)
{
    const auto area = juce::Rectangle<float>(plot.getRight() + 8.0f, plot.getY(), kSliceW, plot.getHeight());
    g.setColour(KhensuTheme::bgRaised.withAlpha(0.6f));
    g.fillRoundedRectangle(area, 4.0f);
    const auto& sg = timeline_->spectrogramDb;
    const int bands = static_cast<int>(sg.front().size());
    const bool fromHover = plot.contains(hover_) && !dragging_;
    const double t = fromHover ? timeAtX(hover_.x) : (playhead_ >= 0.0 ? playhead_ : (viewStart_ + viewEnd_) * 0.5);
    const int f = frameAt(t);
    if (f < 0) return;
    const float floorDb = topDb_ - rangeDb_;
    auto xFor = [&](float db) { return area.getX() + juce::jlimit(0.0f, 1.0f, (db - floorDb) / rangeDb_) * area.getWidth(); };
    auto yFor = [&](int b) { return area.getBottom() - (static_cast<float>(b) + 0.5f) / bands * area.getHeight(); };

    // This moment, filled with the map's own colours.
    juce::Path fill, line;
    fill.startNewSubPath(area.getX(), area.getBottom());
    for (int b = 0; b < bands; ++b) {
        const float x = xFor(sg[static_cast<size_t>(f)][static_cast<size_t>(b)]), y = yFor(b);
        fill.lineTo(x, y);
        if (b == 0) line.startNewSubPath(x, y);
        else line.lineTo(x, y);
    }
    fill.lineTo(area.getX(), area.getY());
    fill.closeSubPath();
    juce::ColourGradient grad(ui::viz::spectrogram(0.15f).withAlpha(0.5f), area.getX(), 0.0f,
                              ui::viz::spectrogram(0.95f).withAlpha(0.85f), area.getRight(), 0.0f, false);
    grad.addColour(0.6, ui::viz::spectrogram(0.6f).withAlpha(0.7f));
    g.setGradientFill(grad);
    g.fillPath(fill);
    g.setColour(ui::viz::spectrogram(0.9f));
    g.strokePath(line, juce::PathStrokeType(1.2f));

    // The song's average spectrum for comparison.
    if (static_cast<int>(meanDb_.size()) == bands) {
        juce::Path mean;
        for (int b = 0; b < bands; ++b) {
            const float x = xFor(meanDb_[static_cast<size_t>(b)]), y = yFor(b);
            if (b == 0) mean.startNewSubPath(x, y);
            else mean.lineTo(x, y);
        }
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.8f));
        const float dashes[] = {2.5f, 2.5f};
        juce::Path dashed;
        juce::PathStrokeType(1.0f).createDashedStroke(dashed, mean, dashes, 2);
        g.fillPath(dashed);
    }
    g.setFont(KhensuTheme::labelFont(9.0f));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText(fromHover ? TRANS("At cursor") : TRANS("At playhead"),
               area.withHeight(13.0f).translated(0.0f, area.getHeight() + 2.0f), juce::Justification::centred, false);
}

void HiResSpectrogramView::drawLegend(juce::Graphics& g, juce::Rectangle<float> plot)
{
    const auto bar = juce::Rectangle<float>(plot.getRight() + 8.0f + kSliceW + 8.0f, plot.getY() + 6.0f, 6.0f,
                                            plot.getHeight() - 12.0f);
    juce::ColourGradient grad(ui::viz::spectrogram(1.0f), 0.0f, bar.getY(), ui::viz::spectrogram(0.0f), 0.0f, bar.getBottom(), false);
    for (int i = 1; i < 8; ++i)
        grad.addColour(i / 8.0, ui::viz::spectrogram(1.0f - i / 8.0f));
    g.setGradientFill(grad);
    g.fillRoundedRectangle(bar, 3.0f);
    g.setFont(KhensuTheme::labelFont(8.5f));
    g.setColour(KhensuTheme::textTertiary);
    const auto lbl = [&](float y, const juce::String& t) {
        g.drawText(t, juce::Rectangle<float>(bar.getRight() + 1.0f, y - 6.0f, kLegendW - 8.0f, 12.0f),
                   juce::Justification::centredLeft, false);
    };
    lbl(bar.getY(), "0");
    lbl(bar.getCentreY(), juce::String(-juce::roundToInt(rangeDb_ / 2.0f)));
    lbl(bar.getBottom(), juce::String(-juce::roundToInt(rangeDb_)));
}

void HiResSpectrogramView::mouseMove(const juce::MouseEvent& e)
{
    hover_ = e.position;
    repaint();
}

void HiResSpectrogramView::mouseExit(const juce::MouseEvent&)
{
    hover_ = {-1.0f, -1.0f};
    repaint();
}

void HiResSpectrogramView::mouseDown(const juce::MouseEvent& e)
{
    dragStartX_ = e.position.x;
    dragViewStart_ = targetStart_;
    dragViewEnd_ = targetEnd_;
    dragging_ = false;
}

void HiResSpectrogramView::mouseDrag(const juce::MouseEvent& e)
{
    const auto plot = plotArea();
    if (timeline_ == nullptr || plot.isEmpty()) return;
    if (!dragging_ && std::abs(e.position.x - dragStartX_) < 3.0f) return;
    dragging_ = true;
    const double span = dragViewEnd_ - dragViewStart_;
    const double dt = -(e.position.x - dragStartX_) / plot.getWidth() * span;
    setTargetView(dragViewStart_ + dt, dragViewEnd_ + dt, false);
}

void HiResSpectrogramView::mouseUp(const juce::MouseEvent& e)
{
    const bool wasDrag = dragging_;
    dragging_ = false;
    if (!wasDrag && plotArea().contains(e.position) && onSeek) onSeek(timeAtX(e.position.x));
    repaint();
}

void HiResSpectrogramView::mouseDoubleClick(const juce::MouseEvent&)
{
    if (timeline_ != nullptr) setTargetView(0.0, timeline_->durationSec, true);
}

void HiResSpectrogramView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (timeline_ == nullptr) return;
    const auto plot = plotArea();
    const double span = targetEnd_ - targetStart_;
    if (std::abs(wheel.deltaX) > std::abs(wheel.deltaY)) { // a horizontal swipe pans
        const double dt = -wheel.deltaX * span * 0.5;
        setTargetView(targetStart_ + dt, targetEnd_ + dt, true);
        return;
    }
    const double anchorFrac = plot.isEmpty() ? 0.5 : juce::jlimit(0.0, 1.0, static_cast<double>((e.position.x - plot.getX()) / plot.getWidth()));
    const double anchor = targetStart_ + anchorFrac * span;
    const double newSpan = span * std::pow(0.8, wheel.deltaY * 4.0);
    setTargetView(anchor - anchorFrac * newSpan, anchor + (1.0 - anchorFrac) * newSpan, true);
}

} // namespace khensu
