#include <khensu/ui/StudioMeters.h>
#include <khensu/ui/KhensuPalette.h>
#include <khensu/ui/StudioStyle.h>

#include <algorithm>

namespace khensu::ui {

namespace {
using studio::formatDb;
using studio::numberFont;

constexpr float kNoSignal = -70.0f;

/// Horizontal bar on `r`: fills 0..`fraction` with up to three colours
/// split at the proportions `lowSplit` / `highSplit` (target band edges).
void fillSegmentedBar(juce::Graphics& g, juce::Rectangle<float> r, float fraction, float lowSplit, float highSplit,
                      juce::Colour below, juce::Colour inside, juce::Colour above, bool vertical)
{
    fraction = juce::jlimit(0.0f, 1.0f, fraction);
    auto span = [&](float from, float to, juce::Colour c) {
        from = juce::jlimit(0.0f, fraction, from);
        to = juce::jlimit(0.0f, fraction, to);
        if (to <= from) return;
        g.setColour(c);
        if (vertical)
            g.fillRect(r.getX(), r.getBottom() - r.getHeight() * to, r.getWidth(), r.getHeight() * (to - from));
        else
            g.fillRect(r.getX() + r.getWidth() * from, r.getY(), r.getWidth() * (to - from), r.getHeight());
    };
    span(0.0f, lowSplit, below);
    span(lowSplit, highSplit, inside);
    span(highSplit, 1.0f, above);
}

void drawTrack(juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour(studio::surfaceRaised());
    g.fillRoundedRectangle(r, 2.0f);
}

void drawLabel(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r, juce::Justification j,
               float size = studio::kLabelSize)
{
    g.setColour(Palette::textMuted);
    g.setFont(studio::labelFont(size));
    g.drawText(text, r, j, false);
}
} // namespace

//==============================================================================
AnimatedMeter::AnimatedMeter()
    : vblank_(this, [this](double timestampSec) {
          const double dt = lastFrame_ < 0.0 ? 1.0 / 60.0 : juce::jlimit(0.0, 0.25, timestampSec - lastFrame_);
          lastFrame_ = timestampSec;
          if (isShowing()) advance(dt);
      })
{
    setOpaque(false);
}

void AnimatedMeter::setStopped(bool stopped)
{
    if (stopped == stopped_) return;
    stopped_ = stopped;
    repaint();
}

juce::Colour AnimatedMeter::ink(juce::Colour c) const noexcept
{
    if (!stopped_) return c;
    // Most of the hue gone and dimmed: the analysed state, not a live reading.
    return c.withSaturation(c.getSaturation() * 0.15f).withMultipliedAlpha(0.7f);
}

//==============================================================================
LoudnessMeter::LoudnessMeter()
{
    setTitle("Loudness meter");
    setDescription("Momentary, short-term and integrated loudness in LUFS. Click the integrated value to reset it.");
}

void LoudnessMeter::setValues(float momentaryLufs, float shortTermLufs, float integratedLufs) noexcept
{
    momentary_ = momentaryLufs;
    shortTerm_ = shortTermLufs;
    if (std::abs(integratedLufs - integrated_) > 0.005f || std::isfinite(integratedLufs) != std::isfinite(integrated_)) {
        integrated_ = integratedLufs;
        repaint(); // the integrated readout has no ballistics
    }
}

void LoudnessMeter::setTarget(float targetLufs, float toleranceLu)
{
    target_ = targetLufs;
    tolerance_ = std::abs(toleranceLu);
    repaint();
}

void LoudnessMeter::setScale(float minLufs, float maxLufs)
{
    if (maxLufs <= minLufs) return;
    minLufs_ = minLufs;
    maxLufs_ = maxLufs;
    repaint();
}

void LoudnessMeter::setCompact(bool compact)
{
    compact_ = compact;
    repaint();
}

void LoudnessMeter::advance(double dt)
{
    const float m0 = mDisplay_.value, s0 = sDisplay_.value, h0 = mHold_.value;
    mDisplay_.update(juce::jmax(-100.0f, std::isfinite(momentary_) ? momentary_ : -100.0f), dt);
    sDisplay_.update(juce::jmax(-100.0f, std::isfinite(shortTerm_) ? shortTerm_ : -100.0f), dt);
    mHold_.update(momentary_, dt);
    if (std::abs(m0 - mDisplay_.value) > 0.01f || std::abs(s0 - sDisplay_.value) > 0.01f
        || std::abs(h0 - mHold_.value) > 0.01f)
        repaint();
}

juce::Rectangle<float> LoudnessMeter::integratedArea() const
{
    auto b = getLocalBounds().toFloat().reduced(6.0f);
    const bool sideBySide = !compact_ && b.getWidth() >= b.getHeight() * 2.2f;
    if (sideBySide) return b.removeFromLeft(juce::jmin(b.getWidth() * 0.42f, 150.0f));
    return b.removeFromTop(juce::jmin(b.getHeight() * 0.55f, compact_ ? 40.0f : 48.0f));
}

void LoudnessMeter::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced(6.0f);
    if (b.isEmpty()) return;
    const bool sideBySide = !compact_ && b.getWidth() >= b.getHeight() * 2.2f;
    auto readout = sideBySide ? b.removeFromLeft(juce::jmin(b.getWidth() * 0.42f, 150.0f))
                              : b.removeFromTop(juce::jmin(b.getHeight() * 0.55f, compact_ ? 40.0f : 48.0f));
    if (sideBySide) b.removeFromLeft(8.0f);

    // Integrated: the largest number on the meter.
    {
        auto r = readout;
        drawLabel(g, "INTEGRATED", r.removeFromTop(14.0f), juce::Justification::topLeft);
        const float size = compact_ ? 22.0f : studio::kDisplaySize;
        auto valueRow = r.removeFromTop(size + 4.0f);
        const bool hasValue = integrated_ > kNoSignal;
        const bool over = hasValue && std::isfinite(target_) && integrated_ > target_ + tolerance_;
        g.setColour(ink(over ? Palette::statusWarn : Palette::textPrimary));
        g.setFont(numberFont(size));
        const auto text = hasValue ? juce::String(integrated_, 1) : juce::String(juce::CharPointer_UTF8("\xe2\x80\x94")); // utf8-exempt(em dash)
        g.drawText(text, valueRow, juce::Justification::centredLeft, false);
        const float textW = juce::GlyphArrangement::getStringWidth(numberFont(size), text);
        drawLabel(g, "LUFS", valueRow.withTrimmedLeft(textW + 4.0f), juce::Justification::centredLeft);
        if (!compact_ && r.getHeight() >= 14.0f) {
            juce::String extra;
            if (std::isfinite(lra_)) extra << "LRA " << juce::String(lra_, 1) << " LU";
            if (std::isfinite(target_)) extra << (extra.isEmpty() ? "" : "   ") << "Target " << juce::String(target_, 1);
            g.setFont(numberFont(studio::kLabelSize));
            g.setColour(Palette::textMuted);
            g.drawText(extra, r.removeFromTop(14.0f), juce::Justification::topLeft, false);
        }
    }

    // Bars: M and S (compact: S only when there is room for one bar).
    auto scaleRow = b.removeFromBottom(12.0f);
    const bool twoBars = b.getHeight() >= 22.0f;
    const float barH = juce::jlimit(4.0f, 14.0f, (b.getHeight() - (twoBars ? 6.0f : 0.0f)) / (twoBars ? 2.0f : 1.0f));
    const float labelW = 14.0f, valueW = 42.0f;
    auto barsArea = b.withSizeKeepingCentre(b.getWidth(), barH * (twoBars ? 2.0f : 1.0f) + (twoBars ? 6.0f : 0.0f));
    auto trackX = barsArea.getX() + labelW, trackW = barsArea.getWidth() - labelW - valueW;
    if (trackW < 10.0f) return;
    auto toX = [&](float lufs) { return trackX + trackW * meter::proportion(lufs, minLufs_, maxLufs_); };

    const float lowSplit = std::isfinite(target_) ? meter::proportion(target_ - tolerance_, minLufs_, maxLufs_) : 1.0f;
    const float highSplit = std::isfinite(target_) ? meter::proportion(target_ + tolerance_, minLufs_, maxLufs_) : 1.0f;

    auto drawRow = [&](juce::Rectangle<float> row, const char* name, float display, float reading, bool hold) {
        drawLabel(g, name, row.removeFromLeft(labelW), juce::Justification::centredLeft);
        auto valueArea = row.removeFromRight(valueW);
        auto track = row;
        drawTrack(g, track);
        fillSegmentedBar(g, track, meter::proportion(display, minLufs_, maxLufs_), lowSplit, highSplit,
                         ink(studio::neutralFill()), ink(Palette::accentTeal), ink(Palette::statusWarn), false);
        if (hold && mHold_.value > minLufs_) {
            g.setColour(Palette::textPrimary.withAlpha(0.8f));
            g.fillRect(toX(mHold_.value) - 1.0f, track.getY(), 2.0f, track.getHeight());
        }
        g.setColour(Palette::textPrimary);
        g.setFont(numberFont(studio::kBodySize));
        g.drawText(reading > kNoSignal ? juce::String(reading, 1) : juce::String(juce::CharPointer_UTF8("\xe2\x80\x94")), // utf8-exempt(em dash)
                   valueArea, juce::Justification::centredRight, false);
    };

    // Target band behind both bars.
    if (std::isfinite(target_)) {
        const float x0 = toX(target_ - tolerance_), x1 = toX(target_ + tolerance_);
        g.setColour(Palette::accentTeal.withAlpha(0.12f));
        g.fillRect(x0, barsArea.getY() - 3.0f, juce::jmax(1.0f, x1 - x0), barsArea.getHeight() + 6.0f);
        g.setColour(Palette::accentTeal.withAlpha(0.7f));
        g.fillRect(toX(target_) - 0.5f, barsArea.getY() - 3.0f, 1.0f, barsArea.getHeight() + 6.0f);
    }

    auto rows = barsArea;
    if (twoBars) {
        drawRow(rows.removeFromTop(barH), "M", mDisplay_.value, momentary_, true);
        rows.removeFromTop(6.0f);
    }
    drawRow(rows.removeFromTop(barH), "S", sDisplay_.value, shortTerm_, !twoBars);

    // Scale ticks every 6 LU.
    g.setFont(numberFont(type::caption));
    // BUG-5: a step that keeps the labels at least ~30 px apart (6, 12, 18 or
    // 36 LU); a track too short for two labels gets none.
    const float span = maxLufs_ - minLufs_;
    float tickStep = 36.0f;
    for (const float step : {6.0f, 12.0f, 18.0f, 36.0f})
        if (trackW * step / span >= 30.0f) {
            tickStep = step;
            break;
        }
    if (trackW * tickStep / span < 30.0f) return;
    for (float lufs = std::ceil(minLufs_ / tickStep) * tickStep; lufs <= maxLufs_ + 0.01f; lufs += tickStep) {
        const float x = toX(lufs);
        g.setColour(studio::hairline());
        g.fillRect(x - 0.5f, barsArea.getBottom() + 1.0f, 1.0f, 3.0f);
        g.setColour(Palette::textMuted);
        g.drawText(juce::String(juce::roundToInt(lufs)), juce::Rectangle<float>(x - 14.0f, scaleRow.getY(), 28.0f, 12.0f),
                   juce::Justification::centred, false);
    }
}

void LoudnessMeter::mouseUp(const juce::MouseEvent& e)
{
    if (!integratedArea().contains(e.position)) return;
    if (onResetIntegrated) onResetIntegrated();
    mHold_.reset();
    repaint();
}

//==============================================================================
TruePeakMeter::TruePeakMeter()
{
    for (auto& v : input_) v = -100.0f;
    for (auto& bar : bars_) {
        bar.floorDb = -100.0f;
        bar.reset();
    }
    for (auto& hold : holds_) {
        hold.floorDb = -100.0f;
        hold.reset();
    }
    setTitle("True peak meter");
    setDescription("True peak per channel in dBTP with the highest value held. Click to reset the hold.");
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void TruePeakMeter::setPeaks(const float* dbtp, int numChannels) noexcept
{
    if (dbtp == nullptr) return;
    numChannels_ = juce::jlimit(1, kMaxChannels, numChannels);
    for (int c = 0; c < numChannels_; ++c) {
        const float v = std::isfinite(dbtp[c]) ? dbtp[c] : -100.0f;
        // Keep the highest value pushed between two frames.
        input_[static_cast<size_t>(c)] = juce::jmax(input_[static_cast<size_t>(c)], v);
        maxHold_.update(v);
    }
}

void TruePeakMeter::setPeaks(float leftDbtp, float rightDbtp) noexcept
{
    const float v[2] = {leftDbtp, rightDbtp};
    setPeaks(v, 2);
}

void TruePeakMeter::setScale(float minDb, float maxDb)
{
    if (maxDb <= minDb) return;
    minDb_ = minDb;
    maxDb_ = maxDb;
    repaint();
}

void TruePeakMeter::resetHold()
{
    maxHold_.reset();
    for (auto& hold : holds_) hold.reset();
    if (onReset) onReset();
    repaint();
}

float TruePeakMeter::displayedPeak(int channel) const noexcept
{
    return channel >= 0 && channel < kMaxChannels ? bars_[static_cast<size_t>(channel)].value : -100.0f;
}

void TruePeakMeter::advance(double dt)
{
    bool moved = false;
    for (int c = 0; c < numChannels_; ++c) {
        const auto i = static_cast<size_t>(c);
        const float before = bars_[i].value, holdBefore = holds_[i].value;
        bars_[i].update(input_[i], dt);
        holds_[i].update(input_[i], dt);
        input_[i] = -100.0f;
        moved = moved || std::abs(before - bars_[i].value) > 0.01f || std::abs(holdBefore - holds_[i].value) > 0.01f;
    }
    if (moved) repaint();
}

void TruePeakMeter::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced(6.0f);
    if (b.isEmpty()) return;
    // Numeric max hold on top: the measured value is the largest text.
    auto top = b.removeFromTop(juce::jmin(40.0f, b.getHeight() * 0.35f));
    drawLabel(g, "TRUE PEAK", top.removeFromTop(13.0f), juce::Justification::topLeft);
    const bool over = maxHold_.hasValue() && maxHold_.value > ceiling_;
    g.setColour(ink(over ? Palette::statusBad : Palette::textPrimary));
    const float size = juce::jlimit(12.0f, studio::kTitleSize, top.getHeight() - 2.0f);
    g.setFont(numberFont(size));
    const auto text = maxHold_.hasValue() ? formatDb(maxHold_.value, 1, -99.0f) : juce::String(juce::CharPointer_UTF8("\xe2\x80\x94")); // utf8-exempt(em dash)
    g.drawText(text, top, juce::Justification::centredLeft, false);
    const float textW = juce::GlyphArrangement::getStringWidth(numberFont(size), text);
    drawLabel(g, "dBTP", top.withTrimmedLeft(textW + 3.0f), juce::Justification::centredLeft);

    b.removeFromTop(4.0f);
    auto labels = b.removeFromBottom(12.0f);
    const int n = numChannels_;
    const float gap = 3.0f;
    const float barW = juce::jmax(2.0f, (b.getWidth() - gap * static_cast<float>(n - 1)) / static_cast<float>(n));
    const float ceilP = meter::proportion(ceiling_, minDb_, maxDb_);
    const float warnP = meter::proportion(ceiling_ - 3.0f, minDb_, maxDb_);
    for (int c = 0; c < n; ++c) {
        const auto i = static_cast<size_t>(c);
        juce::Rectangle<float> bar(b.getX() + static_cast<float>(c) * (barW + gap), b.getY(), barW, b.getHeight());
        drawTrack(g, bar);
        fillSegmentedBar(g, bar, meter::proportion(bars_[i].value, minDb_, maxDb_), warnP, ceilP,
                         ink(studio::neutralFill()), ink(Palette::statusWarn), ink(Palette::statusBad), true);
        if (holds_[i].value > minDb_) {
            const float y = bar.getBottom() - bar.getHeight() * meter::proportion(holds_[i].value, minDb_, maxDb_);
            g.setColour(ink(holds_[i].value > ceiling_ ? Palette::statusBad : Palette::textPrimary.withAlpha(0.85f)));
            g.fillRect(bar.getX(), y - 1.0f, bar.getWidth(), 2.0f);
        }
        if (n <= 2)
            drawLabel(g, c == 0 ? "L" : "R", juce::Rectangle<float>(bar.getX(), labels.getY(), bar.getWidth(), 12.0f),
                      juce::Justification::centred);
    }
    // Ceiling line across the bars.
    const float cy = b.getBottom() - b.getHeight() * ceilP;
    g.setColour(Palette::statusBad.withAlpha(0.6f));
    g.fillRect(b.getX(), cy - 0.5f, b.getWidth(), 1.0f);
}

void TruePeakMeter::mouseUp(const juce::MouseEvent&)
{
    resetHold();
}

//==============================================================================
CorrelationMeter::CorrelationMeter()
{
    setTitle("Correlation meter");
    setDescription("Phase correlation between left and right, from -1 (out of phase) to +1 (mono).");
}

void CorrelationMeter::setCorrelation(float correlation) noexcept
{
    if (std::isfinite(correlation)) input_ = juce::jlimit(-1.0f, 1.0f, correlation);
}

void CorrelationMeter::advance(double dt)
{
    const float before = needle_.value, minBefore = minHold_.value;
    needle_.update(input_, dt);
    // The lowest recent reading: drops instantly, recovers over ~1.5 s.
    minHold_.update(input_, dt);
    if (std::abs(before - needle_.value) > 0.002f || std::abs(minBefore - minHold_.value) > 0.002f) repaint();
}

void CorrelationMeter::paint(juce::Graphics& g)
{
    // BUG-5: the caption row, the track and the scale each get their own
    // strip; a short meter drops the scale, then the caption, instead of
    // drawing them over each other.
    auto b = getLocalBounds().toFloat().reduced(4.0f, 2.0f);
    if (b.getWidth() < 40.0f || b.getHeight() < 6.0f) return;
    const float value = needle_.value;
    const bool withCaption = b.getHeight() >= 26.0f;
    const bool withScale = b.getHeight() >= 40.0f;
    if (withCaption) {
        auto top = b.removeFromTop(14.0f);
        const auto valueText = (value >= 0.0f ? "+" : "") + juce::String(value, 2);
        const float valueW = juce::GlyphArrangement::getStringWidth(numberFont(studio::kBodySize), valueText) + 6.0f;
        g.setColour(ink(value < -0.5f ? Palette::statusBad : value < 0.0f ? Palette::statusWarn : Palette::textPrimary));
        g.setFont(numberFont(studio::kBodySize));
        g.drawText(valueText, top.removeFromRight(valueW), juce::Justification::centredRight, false);
        if (top.getWidth() > 30.0f)
            drawLabel(g, top.getWidth() >= 84.0f ? "CORRELATION" : "CORR", top, juce::Justification::centredLeft);
        b.removeFromTop(3.0f);
    }
    juce::Rectangle<float> scale;
    if (withScale) scale = b.removeFromBottom(12.0f);
    auto track = b.withSizeKeepingCentre(b.getWidth(), juce::jlimit(3.0f, 10.0f, b.getHeight() - 6.0f));
    drawTrack(g, track);
    auto toX = [&](float c) { return track.getX() + track.getWidth() * (c + 1.0f) * 0.5f; };
    const float x0 = toX(0.0f), xv = toX(value);
    g.setColour(ink(value < -0.5f   ? Palette::statusBad
                    : value < 0.0f ? Palette::statusWarn
                                   : Palette::accentTeal.withAlpha(0.65f)));
    g.fillRect(juce::jmin(x0, xv), track.getY(), std::abs(xv - x0), track.getHeight());
    // Lowest recent value, faint.
    if (minHold_.value < value - 0.02f) {
        g.setColour(Palette::textMuted.withAlpha(0.7f));
        g.fillRect(toX(minHold_.value) - 0.5f, track.getY(), 1.0f, track.getHeight());
    }
    g.setColour(studio::hairline().withAlpha(0.4f));
    g.fillRect(x0 - 0.5f, track.getY() - 2.0f, 1.0f, track.getHeight() + 4.0f);
    g.setColour(ink(Palette::textPrimary));
    g.fillRoundedRectangle(xv - 1.5f, track.getY() - 2.0f, 3.0f, track.getHeight() + 4.0f, 1.0f);

    if (scale.isEmpty()) return;
    g.setFont(numberFont(type::caption));
    g.setColour(Palette::textMuted);
    for (float c : {-1.0f, 0.0f, 1.0f}) {
        const juce::String t = c < 0.0f ? "-1" : c > 0.0f ? "+1" : "0";
        const auto just = c < 0.0f   ? juce::Justification::centredLeft
                          : c > 0.0f ? juce::Justification::centredRight
                                     : juce::Justification::centred;
        g.drawText(t, juce::Rectangle<float>(toX(c) - (c < 0.0f ? 0.0f : c > 0.0f ? 24.0f : 12.0f), scale.getY(), 24.0f, 12.0f),
                   just, false);
    }
}

//==============================================================================
Vectorscope::Vectorscope()
{
    setTitle("Vectorscope");
    setDescription("Stereo image: mono sits on the vertical axis, wide material spreads sideways, "
                   "out-of-phase material lies along the horizontal axis.");
    scratchL_.assign(8192, 0.0f);
    scratchR_.assign(8192, 0.0f);
}

void Vectorscope::setSource(StereoSampleRing* ring) noexcept
{
    source_ = ring;
    this->ring().clear();
}

void Vectorscope::pushSamples(const float* left, const float* right, int count) noexcept
{
    ownRing_.push(left, right, count);
}

void Vectorscope::resized()
{
    const auto side = juce::jmin(getWidth(), getHeight()) - 12;
    if (side <= 0) {
        trail_ = {};
        return;
    }
    const float scale = juce::jmax(1.0f, juce::Component::getApproximateScaleFactorForComponent(this));
    const int px = juce::roundToInt(static_cast<float>(side) * scale);
    trail_ = juce::Image(juce::Image::ARGB, px, px, true);
}

void Vectorscope::visibilityChanged()
{
    // Opening the scope shows what plays now, not what queued while hidden.
    if (isVisible()) ring().clear();
}

void Vectorscope::advance(double dt)
{
    auto& source = ring();
    int total = 0, drawn = 0;
    if (!trail_.isValid()) {
        source.clear();
        return;
    }
    // Fade what is there: persistence is per 1/60 s, scaled to this frame.
    const float keep = std::pow(persistence_, static_cast<float>(dt * 60.0));
    trail_.multiplyAllAlphas(keep);

    const float size = static_cast<float>(trail_.getWidth());
    const float half = size * 0.5f;
    const float dot = juce::jmax(1.0f, size / 160.0f);
    juce::Graphics ig(trail_);
    ig.setColour(ink(Palette::accentTeal).withAlpha(0.55f));
    float framePeak = 0.0f;
    for (;;) {
        const int n = source.pop(scratchL_.data(), scratchR_.data(), static_cast<int>(scratchL_.size()));
        if (n <= 0) break;
        total += n;
        // Draw at most ~2048 points a frame; decimate evenly beyond that.
        const int step = juce::jmax(1, n / 2048);
        for (int i = 0; i < n; i += step) {
            const float l = scratchL_[static_cast<size_t>(i)], r = scratchR_[static_cast<size_t>(i)];
            if (!std::isfinite(l) || !std::isfinite(r)) continue;
            framePeak = juce::jmax(framePeak, std::abs(l), std::abs(r));
            // Mid up, side across; left-only leans to the upper left.
            const float x = (r - l) * 0.70710678f * scale_;
            const float y = (l + r) * 0.70710678f * scale_;
            ig.fillRect(half + x * half * 0.95f - dot * 0.5f, half - y * half * 0.95f - dot * 0.5f, dot, dot);
            ++drawn;
        }
    }
    lastFrameCount_ = drawn;
    // Gentle auto-gain so quiet material still fills the scope (1x..8x).
    if (framePeak > 1.0e-4f) {
        const float wanted = juce::jlimit(1.0f, 8.0f, 0.8f / framePeak);
        scale_ += (wanted - scale_) * (wanted < scale_ ? 0.5f : 0.02f);
    }
    juce::ignoreUnused(total);
    repaint();
}

void Vectorscope::paint(juce::Graphics& g)
{
    const auto side = static_cast<float>(juce::jmin(getWidth(), getHeight()) - 12);
    if (side <= 0.0f) return;
    auto square = getLocalBounds().toFloat().withSizeKeepingCentre(side, side);
    g.setColour(studio::surface());
    g.fillEllipse(square);
    g.setColour(studio::hairline());
    g.drawEllipse(square.reduced(0.5f), 1.0f);
    const auto c = square.getCentre();
    const float r = side * 0.5f;
    // Axes: M vertical, S horizontal; L and R diagonals.
    g.drawLine(c.x, square.getY(), c.x, square.getBottom(), 1.0f);
    g.drawLine(square.getX(), c.y, square.getRight(), c.y, 1.0f);
    const float d = r * 0.70710678f;
    g.drawLine(c.x - d, c.y - d, c.x + d, c.y + d, 1.0f);
    g.drawLine(c.x + d, c.y - d, c.x - d, c.y + d, 1.0f);
    if (trail_.isValid()) g.drawImage(trail_, square, juce::RectanglePlacement::stretchToFit);
    g.setColour(Palette::textMuted);
    g.setFont(studio::labelFont(type::caption));
    g.drawText("L", juce::Rectangle<float>(c.x - d - 14.0f, c.y - d - 14.0f, 12.0f, 12.0f), juce::Justification::centred);
    g.drawText("R", juce::Rectangle<float>(c.x + d + 2.0f, c.y - d - 14.0f, 12.0f, 12.0f), juce::Justification::centred);
    g.drawText("M", juce::Rectangle<float>(c.x + 2.0f, square.getY() + 1.0f, 12.0f, 12.0f), juce::Justification::centredLeft);
}

} // namespace khensu::ui
