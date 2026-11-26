// desktop-app/Source/ui/shared/TimelineComponent.cpp
#include "TimelineComponent.h"
#include "../../IAppController.h"
#include "KhensuLookAndFeel.h"
#include "KhensuTheme.h"
#include "ChartKitDesktop.h"
#include <khensu/ui/KhensuPalette.h>

namespace khensu {

TimelineComponent::TimelineComponent(IAppController* controller)
    : controller_(controller), thumbnailCache_(5), thumbnail_(512, formatManager_, thumbnailCache_)
{
    formatManager_.registerBasicFormats();
    thumbnail_.addChangeListener(this);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setTitle(TRANS("Song timeline"));
    if (controller_ != nullptr) vblank_ = std::make_unique<juce::VBlankAttachment>(this, [this] { onVBlank(); });
}

TimelineComponent::~TimelineComponent()
{
    vblank_.reset();
    thumbnail_.removeChangeListener(this);
}

void TimelineComponent::setSpan(float t)
{
    total_ = t;
    repaint();
}
void TimelineComponent::setSections(std::vector<TimelineSection> s)
{
    sections_ = std::move(s);
    repaint();
}
void TimelineComponent::setMarkers(std::vector<TimelineMarker> m)
{
    markers_ = std::move(m);
    repaint();
}
void TimelineComponent::setBeats(std::vector<float> b, std::vector<float> d)
{
    beats_ = std::move(b);
    downbeats_ = std::move(d);
    repaint();
}

void TimelineComponent::setAudioFile(const juce::File& file)
{
    waveformCacheNeedsRepaint_ = true;
    thumbnail_.setSource(new juce::FileInputSource(file));
}

void TimelineComponent::setRecommendations(std::vector<Recommendation> recs)
{
    recs_ = std::move(recs);
    repaint();
}

juce::Rectangle<int> TimelineComponent::playheadStrip(float sec) const
{
    // Matches paint(): a 2 px line plus the 10 px triangular head in the
    // waveform row (the lower half, above the time axis). Padded a pixel
    // for antialiasing.
    const int contentH = getHeight() - kAxisH;
    const int sectionH = contentH / 2;
    const int x = juce::roundToInt(xFor(sec));
    return {x - 7, sectionH, 14, contentH - sectionH};
}

void TimelineComponent::onVBlank()
{
    if (controller_ == nullptr) return;
    const float pos = static_cast<float>(controller_->playback().getPlaybackPosition());
    if (std::abs(playheadSec_ - pos) <= 0.001f) return;
    // Repaint only when the playhead moves to another pixel column, and then
    // only the old and new strips (dirty-rect repaint).
    const bool wasShown = playheadSec_ >= 0.0f;
    const auto oldStrip = wasShown ? playheadStrip(playheadSec_) : juce::Rectangle<int>();
    const bool samePixel = wasShown && pos >= 0.0f && juce::roundToInt(xFor(pos)) == juce::roundToInt(xFor(playheadSec_));
    playheadSec_ = pos;
    if (samePixel) return;
    if (!oldStrip.isEmpty()) repaint(oldStrip);
    if (pos >= 0.0f) repaint(playheadStrip(pos));
}

void TimelineComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &thumbnail_) {
        waveformCacheNeedsRepaint_ = true;
        repaint();
    }
}

float TimelineComponent::xFor(float sec) const
{
    if (total_ <= 0.0f) return 0.0f;
    return juce::jlimit(0.0f, 1.0f, sec / total_) * (float)getWidth();
}

float TimelineComponent::timeForX(float x) const
{
    if (getWidth() <= 0) return 0.0f;
    return juce::jlimit(0.0f, total_, (x / (float)getWidth()) * total_);
}

juce::Colour TimelineComponent::lufsColour(float lufs) const noexcept
{
    // VIS-4: loudness is a magnitude - the sequential palette over the
    // standard loudness axis (-36 .. 0 LUFS), never the severity colours.
    // The value is written beside it.
    const auto axis = ui::chart::scales::lufs();
    return KhensuTheme::vizSequential(static_cast<float>(axis.norm(lufs)));
}

ui::chart::Axis TimelineComponent::timeAxis() const
{
    return ui::chart::scales::time(total_);
}

juce::Rectangle<int> TimelineComponent::axisRow() const
{
    return getLocalBounds().removeFromBottom(kAxisH);
}

juce::String TimelineComponent::summary() const
{
    // VIS-8: "Song timeline, 3:12: 6 sections: Intro -22.4 LUFS, ...; 2 findings at a moment".
    if (total_ <= 0.0f) return TRANS("Song timeline: no track yet.");
    juce::String s = TRANS("Song timeline") + ", " + timeAxis().label(total_) + ": ";
    juce::StringArray parts;
    for (const auto& sec : sections_)
        parts.add(sec.label + (sec.lufs_integrated != 0.0f ? " " + juce::String(sec.lufs_integrated, 1) + " LUFS" : juce::String()));
    s << (parts.isEmpty() ? TRANS("no sections") : juce::String(parts.size()) + " " + TRANS("sections") + ": " + parts.joinIntoString(", "));
    int timed = 0;
    for (const auto& rec : recs_)
        if (rec.time_range.has_value() || !rec.section.empty()) ++timed;
    if (timed > 0) s << "; " << timed << " " << TRANS("findings at a moment");
    if (!markers_.empty()) s << "; " << static_cast<int>(markers_.size()) << " " << TRANS("hooks");
    return s + ".";
}

std::unique_ptr<juce::AccessibilityHandler> TimelineComponent::createAccessibilityHandler()
{
    return ui::chart::makeChartAccessibilityHandler(*this, [this] { return summary(); });
}

void TimelineComponent::paint(juce::Graphics& g)
{
    const auto style = desktopChartStyle();
    auto b = getLocalBounds();
    const auto axisArea = b.removeFromBottom(kAxisH);
    const int sectionH = b.getHeight() / 2;
    auto sectionRow = b.removeFromTop(sectionH);
    // b is now the waveform / marker row

    // --- Section stripes (loudness in the sequential palette, the value written) ---
    for (size_t i = 0; i < sections_.size(); ++i) {
        const float x0 = xFor(sections_[i].start_sec);
        const float x1 = (i + 1 < sections_.size()) ? xFor(sections_[i + 1].start_sec) : (float)getWidth();
        juce::Rectangle<float> r((float)sectionRow.getX() + x0, (float)sectionRow.getY(), x1 - x0,
                                 (float)sectionRow.getHeight());

        // Base fill alternates brightness
        juce::Colour base = (i % 2 == 0) ? KhensuTheme::bgSurface : KhensuTheme::bgSurface.brighter(0.07f);
        g.setColour(base);
        g.fillRect(r.reduced(1.0f, 2.0f));

        if (sections_[i].lufs_integrated != 0.0f) {
            g.setColour(lufsColour(sections_[i].lufs_integrated));
            g.fillRect(r.withWidth(3.0f).reduced(0.0f, 2.0f));
        }

        // Section label (top portion) + LUFS reading (bottom)
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(ui::Palette::uiFont(style.fontSize));
        if (sections_[i].lufs_integrated != 0.0f) {
            auto labelR = r.withHeight(r.getHeight() * 0.55f);
            auto lufsR = r.withTrimmedTop(r.getHeight() * 0.55f);
            g.drawText(sections_[i].label, labelR.toNearestInt(), juce::Justification::centred, true);
            g.setColour(KhensuTheme::textPrimary);
            g.setFont(ui::Palette::uiFont(style.fontSize * 0.9f));
            g.drawText(juce::String(sections_[i].lufs_integrated, 1) + " LUFS", lufsR.toNearestInt(),
                       juce::Justification::centred, true);
        } else {
            g.drawText(sections_[i].label, r.toNearestInt(), juce::Justification::centred, true);
        }
    }

    // --- Waveform thumbnail (via Image Cache) ---
    if (waveformCacheNeedsRepaint_ || waveformCache_.getWidth() != b.getWidth() ||
        waveformCache_.getHeight() != b.getHeight()) {
        recreateWaveformCache(b);
    }
    g.drawImageAt(waveformCache_, b.getX(), b.getY());

    // --- Loudness mapping: the standard loudness axis (VIS-3) ---
    const auto lufsAxis = ui::chart::scales::lufs();
    auto lufsToY = [&](float lufs) -> float {
        const float ratio = juce::jlimit(0.0f, 1.0f, static_cast<float>(lufsAxis.norm(lufs)));
        return (float)b.getBottom() - ratio * (float)b.getHeight();
    };

    // --- Shaded target corridor (-16 to -11 LUFS, streaming genre targets) ---
    {
        float targetTop = lufsToY(-11.0f);
        float targetBottom = lufsToY(-16.0f);
        g.setColour(KhensuTheme::vizSpreadInner);
        g.fillRect((float)b.getX(), targetTop, (float)b.getWidth(), targetBottom - targetTop);
        g.setColour(style.axis);
        g.drawHorizontalLine(juce::roundToInt(targetTop), (float)b.getX(), (float)b.getRight());
        g.drawHorizontalLine(juce::roundToInt(targetBottom), (float)b.getX(), (float)b.getRight());
        g.setColour(style.muted);
        g.setFont(ui::Palette::uiFont(style.fontSize * 0.85f));
        g.drawText("-16 .. -11 LUFS", juce::Rectangle<float>((float)b.getRight() - 96.0f, targetTop - 12.0f, 92.0f, 11.0f),
                   juce::Justification::centredRight, false);
    }

    // --- Excessive limiting / squashed dynamics: a finding, so the severity
    // colour - with its name written on it (VIS-4) ---
    for (size_t i = 0; i < sections_.size(); ++i) {
        if (sections_[i].lufs_integrated != 0.0f) {
            const bool tooLoud = sections_[i].lufs_integrated > -11.0f;
            const bool tooSquashed = sections_[i].crest_db > 0.0f && sections_[i].crest_db < 6.5f;
            if (tooLoud || tooSquashed) {
                float rx0 = xFor(sections_[i].start_sec);
                float rx1 = (i + 1 < sections_.size()) ? xFor(sections_[i + 1].start_sec) : (float)getWidth();
                g.setColour(KhensuTheme::accentRed.withAlpha(0.13f));
                g.fillRect((float)b.getX() + rx0, (float)b.getY(), rx1 - rx0, (float)b.getHeight());
                g.setColour(KhensuTheme::accentRed);
                g.setFont(ui::Palette::uiFont(style.fontSize * 0.85f).boldened());
                g.drawText(tooSquashed ? TRANS("Squashed") : TRANS("Hot limiting"), (int)(rx0 + b.getX() + 4),
                           (int)(b.getY() + 2), (int)(rx1 - rx0 - 8), 11, juce::Justification::centred);
            }
        }
    }

    // --- Drag-selection loop ---
    if (loopStartSec_ >= 0.0f && loopEndSec_ >= 0.0f) {
        float rx0 = xFor(std::min(loopStartSec_, loopEndSec_));
        float rx1 = xFor(std::max(loopStartSec_, loopEndSec_));
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.18f));
        g.fillRect((float)b.getX() + rx0, (float)b.getY(), rx1 - rx0, (float)b.getHeight());
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.6f));
        g.drawVerticalLine(juce::roundToInt(b.getX() + rx0), (float)b.getY(), (float)b.getBottom());
        g.drawVerticalLine(juce::roundToInt(b.getX() + rx1), (float)b.getY(), (float)b.getBottom());
    }

    // --- Section loudness line ---
    {
        juce::Path loudnessPath;
        bool firstPoint = true;
        for (size_t i = 0; i < sections_.size(); ++i) {
            if (sections_[i].lufs_integrated == 0.0f) continue;
            const float start = sections_[i].start_sec;
            const float end = (i + 1 < sections_.size()) ? sections_[i + 1].start_sec : total_;
            const float x = (float)b.getX() + xFor((start + end) / 2.0f);
            const float y = lufsToY(sections_[i].lufs_integrated);
            if (firstPoint) loudnessPath.startNewSubPath(x, y);
            else loudnessPath.lineTo(x, y);
            firstPoint = false;
        }
        if (!loudnessPath.isEmpty()) {
            g.setColour(KhensuTheme::vizCategory(0));
            g.strokePath(loudnessPath, juce::PathStrokeType(2.0f));
        }
    }

    // --- Section division lines with pin heads ---
    for (size_t i = 0; i < sections_.size(); ++i) {
        const float sx = xFor(sections_[i].start_sec);
        g.setColour(KhensuTheme::textPrimary.withAlpha(0.45f));
        g.drawVerticalLine(juce::roundToInt(sx), (float)b.getY(), (float)b.getBottom());
        g.setColour(KhensuTheme::textPrimary);
        g.fillEllipse(sx - 3.0f, (float)b.getY() - 3.0f, 6.0f, 6.0f);
    }

    // --- Playhead (the chart kit's playhead colour) ---
    if (playheadSec_ >= 0.0f) {
        const float px = xFor(playheadSec_);
        g.setColour(style.playhead);
        g.fillRect((float)b.getX() + px - 1.0f, (float)b.getY(), 2.0f, (float)b.getHeight());
        juce::Path head;
        head.addTriangle((float)b.getX() + px, (float)b.getY() + 6.0f, (float)b.getX() + px - 5.0f, (float)b.getY(),
                         (float)b.getX() + px + 5.0f, (float)b.getY());
        g.fillPath(head);
    }

    // --- Findings at a moment (severity colours: these are findings) ---
    auto getRecTime = [this](const Recommendation& rec) -> float {
        if (rec.time_range.has_value()) return rec.time_range->start_sec;
        if (!rec.section.empty()) {
            for (const auto& s : sections_) {
                if (juce::String(s.label).equalsIgnoreCase(juce::String(rec.section))) return s.start_sec;
            }
        }
        return -1.0f;
    };

    const Recommendation* hoveredRec = nullptr;
    float hoveredRecTime = -1.0f;

    for (const auto& rec : recs_) {
        const float tStart = getRecTime(rec);
        if (tStart < 0.0f) continue;

        const juce::Colour markerCol = rec.severity >= 66.0f   ? KhensuTheme::accentRed
                                       : rec.severity >= 33.0f ? KhensuTheme::accentAmber
                                                               : KhensuTheme::accentBlue;
        if (rec.time_range.has_value() && rec.time_range->end_sec > rec.time_range->start_sec) {
            const float rx0 = xFor(rec.time_range->start_sec);
            const float rx1 = xFor(rec.time_range->end_sec);
            g.setColour(markerCol.withAlpha(0.10f));
            g.fillRect((float)b.getX() + rx0, (float)b.getY(), rx1 - rx0, (float)b.getHeight());
        }
        const float mx = xFor(tStart);
        const float my = (float)b.getY() + 10.0f;
        g.setColour(markerCol);
        juce::Path p;
        p.addTriangle(mx, my - 6.0f, mx - 5.0f, my + 4.0f, mx + 5.0f, my + 4.0f);
        g.fillPath(p);
        g.setColour(KhensuTheme::bgVoid);
        g.setFont(ui::Palette::uiFont(type::caption).boldened());
        g.drawText("!", (int)mx - 4, (int)my - 4, 8, 8, juce::Justification::centred);

        if (hoverTimeSec_ >= 0.0f && std::abs(hoverTimeSec_ - tStart) < 3.5f) {
            hoveredRec = &rec;
            hoveredRecTime = tStart;
        }
    }

    // --- Hook markers (strength-proportional bars) ---
    for (const auto& m : markers_) {
        const float x0 = xFor(m.start_sec);
        const float x1 = xFor(m.end_sec);
        const float h = juce::jlimit(0.0f, 1.0f, m.strength) * (float)b.getHeight();
        g.setColour(KhensuTheme::vizCategory(2).withAlpha(0.60f));
        g.fillRect((float)b.getX() + x0, (float)b.getBottom() - h, juce::jmax(2.0f, x1 - x0), h);
    }

    // --- Beat ticks ---
    const float beatH = b.getHeight() * 0.25f;
    g.setColour(KhensuTheme::textSecondary.withAlpha(0.4f));
    for (float sec : beats_)
        g.fillRect((float)b.getX() + xFor(sec), (float)b.getBottom() - beatH, 1.0f, beatH);
    const float downH = b.getHeight() * 0.45f;
    g.setColour(KhensuTheme::textSecondary.withAlpha(0.75f));
    for (float sec : downbeats_)
        g.fillRect((float)b.getX() + xFor(sec) - 0.5f, (float)b.getBottom() - downH, 2.0f, downH);

    // --- The time axis (ChartKit ticks and labels) ---
    if (total_ > 0.0f) {
        const auto axis = timeAxis();
        g.setColour(style.axis);
        g.drawHorizontalLine(axisArea.getY(), 0.0f, (float)getWidth());
        g.setFont(ui::Palette::uiFont(style.fontSize));
        for (const double v : axis.ticks(std::max(2, getWidth() / 70))) {
            const float x = xFor(static_cast<float>(v));
            g.setColour(style.grid);
            g.drawVerticalLine(juce::roundToInt(x), (float)b.getY(), (float)b.getBottom());
            g.setColour(style.muted);
            auto r = juce::Rectangle<float>(x - 30.0f, (float)axisArea.getY() + 1.0f, 60.0f, (float)kAxisH - 1.0f);
            r = r.constrainedWithin(axisArea.toFloat());
            g.drawText(axis.label(v), r, juce::Justification::centredTop, false);
        }
    }

    // --- Hover: crosshair and the ChartKit readout ---
    if (hoverTimeSec_ >= 0.0f) {
        const float hx = xFor(hoverTimeSec_);
        g.setColour(style.crosshair);
        g.drawVerticalLine(juce::roundToInt(hx), 0.0f, (float)b.getBottom());

        ui::chart::Readout r;
        if (hoveredRec != nullptr) {
            r.title = timeAxis().label(hoveredRecTime, true);
            const juce::Colour mCol = hoveredRec->severity >= 66.0f   ? KhensuTheme::accentRed
                                      : hoveredRec->severity >= 33.0f ? KhensuTheme::accentAmber
                                                                      : KhensuTheme::accentBlue;
            r.lines.push_back({juce::String(hoveredRec->what), mCol});
        } else {
            r.title = timeAxis().label(hoverTimeSec_, true);
            for (size_t i = 0; i < sections_.size(); ++i) {
                const float nextStart = (i + 1 < sections_.size()) ? sections_[i + 1].start_sec : total_;
                if (hoverTimeSec_ >= sections_[i].start_sec && hoverTimeSec_ < nextStart) {
                    r.lines.push_back({sections_[i].label, KhensuTheme::textSecondary});
                    if (sections_[i].lufs_integrated != 0.0f) {
                        const float dev = sections_[i].lufs_integrated - (-14.0f); // the streaming reference
                        r.lines.push_back({juce::String(sections_[i].lufs_integrated, 1) + " LUFS (" + (dev >= 0.0f ? "+" : "") +
                                               juce::String(dev, 1) + " LU " + TRANS("vs -14") + ")",
                                           lufsColour(sections_[i].lufs_integrated)});
                    }
                    if (sections_[i].crest_db > 0.0f)
                        r.lines.push_back({TRANS("Crest") + " " + juce::String(sections_[i].crest_db, 1) + " dB",
                                           KhensuTheme::textSecondary});
                    break;
                }
            }
            r.lines.push_back({TRANS("Click to play from here"), style.muted});
        }
        ui::chart::ChartView::paintReadout(g, r, {hx, (float)b.getY() + 8.0f}, getLocalBounds().toFloat(), style);
    }
}

// ---------------------------------------------------------------------------
// Mouse events
// ---------------------------------------------------------------------------

void TimelineComponent::mouseMove(const juce::MouseEvent& e)
{
    if (total_ <= 0.0f) return;
    hoverTimeSec_ = timeForX((float)e.x);
    repaint();
}

void TimelineComponent::mouseExit(const juce::MouseEvent&)
{
    hoverTimeSec_ = -1.0f;
    repaint();
}

void TimelineComponent::seekTo(float sec)
{
    // VIS-1: the seek path every time chart uses - move there and play.
    if (controller_ != nullptr) chart_transport::playFrom(controller_->playback(), sec);
    else if (seekCallback_) seekCallback_(sec);
}

void TimelineComponent::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (total_ <= 0.0f) return;
    seekTo(timeForX((float)e.x));
}

void TimelineComponent::mouseDown(const juce::MouseEvent& e)
{
    if (total_ <= 0.0f) return;
    loopStartSec_ = timeForX((float)e.x);
    loopEndSec_ = loopStartSec_;
    isDraggingLoop_ = true;
    repaint();
}

void TimelineComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (total_ <= 0.0f || !isDraggingLoop_) return;
    loopEndSec_ = timeForX((float)e.x);
    repaint();
}

void TimelineComponent::mouseUp(const juce::MouseEvent& e)
{
    if (total_ <= 0.0f || !isDraggingLoop_) return;
    isDraggingLoop_ = false;

    // Swap if dragged backwards
    if (loopStartSec_ > loopEndSec_) {
        std::swap(loopStartSec_, loopEndSec_);
    }

    // A click (no real drag) plays from there; a tiny selection is no loop.
    if (loopEndSec_ - loopStartSec_ < 0.2f) {
        loopStartSec_ = -1.0f;
        loopEndSec_ = -1.0f;
        if (!e.mouseWasDraggedSinceMouseDown()) seekTo(timeForX((float)e.x));
    }
    repaint();
}

void TimelineComponent::recreateWaveformCache(juce::Rectangle<int> bounds)
{
    if (bounds.isEmpty()) return;

    waveformCache_ = juce::Image(juce::Image::ARGB, bounds.getWidth(), bounds.getHeight(), true);
    juce::Graphics g(waveformCache_);

    juce::Rectangle<int> b(0, 0, bounds.getWidth(), bounds.getHeight());

    if (thumbnail_.getNumChannels() > 0) {
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.30f));
        thumbnail_.drawChannels(g, b.reduced(1, 1), 0.0, thumbnail_.getTotalLength(), 1.0f);
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.65f));
        thumbnail_.drawChannel(g, b.reduced(1, 1), 0.0, thumbnail_.getTotalLength(), 0, 0.95f);
    } else {
        g.setColour(KhensuTheme::bgSurface.brighter(0.06f));
        g.fillRect(b);
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.35f));
        g.setFont(ui::Palette::uiFont(type::caption));
        g.drawText(TRANS("No audio file loaded"), b, juce::Justification::centred, false);
    }

    waveformCacheNeedsRepaint_ = false;
}

} // namespace khensu
