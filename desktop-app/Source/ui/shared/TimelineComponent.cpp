// desktop-app/Source/ui/shared/TimelineComponent.cpp
#include "TimelineComponent.h"
#include "../../IAppController.h"
#include "KhensuLookAndFeel.h"
#include "KhensuTheme.h"

namespace khensu {

TimelineComponent::TimelineComponent(IAppController* controller)
    : controller_(controller), thumbnailCache_(5), thumbnail_(512, formatManager_, thumbnailCache_)
{
    formatManager_.registerBasicFormats();
    thumbnail_.addChangeListener(this);
    setMouseCursor(juce::MouseCursor::CrosshairCursor);
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
    // waveform row (the lower half). Padded a pixel for antialiasing.
    const int sectionH = getHeight() / 2;
    const int x = juce::roundToInt(xFor(sec));
    return {x - 7, sectionH, 14, getHeight() - sectionH};
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
    // Map LUFS to a colour gradient:
    //  >= -9  LUFS : red (too loud, likely clipping headroom lost)
    //  -14    LUFS : amber (Spotify/streaming target)
    //  -18    LUFS : green (good pre-master level)
    //  <= -24 LUFS : blue (very quiet, may need gain)
    if (lufs >= -9.0f) return KhensuTheme::accentRed;  // red
    if (lufs >= -14.0f) return KhensuTheme::accentAmber; // amber
    if (lufs >= -18.0f) return KhensuTheme::accentGreen; // green
    if (lufs >= -24.0f) return KhensuTheme::accentTeal; // teal
    return KhensuTheme::accentBlue;                     // blue (very quiet)
}

void TimelineComponent::paint(juce::Graphics& g)
{
    auto b = getLocalBounds();
    const int sectionH = b.getHeight() / 2;
    auto sectionRow = b.removeFromTop(sectionH);
    // b is now the waveform / marker row

    // --- Section stripes (LUFS-coloured) ---
    for (size_t i = 0; i < sections_.size(); ++i) {
        const float x0 = xFor(sections_[i].start_sec);
        const float x1 = (i + 1 < sections_.size()) ? xFor(sections_[i + 1].start_sec) : (float)getWidth();
        juce::Rectangle<float> r((float)sectionRow.getX() + x0, (float)sectionRow.getY(), x1 - x0,
                                 (float)sectionRow.getHeight());

        // Base fill alternates brightness
        juce::Colour base = (i % 2 == 0) ? KhensuTheme::bgSurface : KhensuTheme::bgSurface.brighter(0.07f);
        g.setColour(base);
        g.fillRect(r.reduced(1.0f, 2.0f));

        // LUFS colour indicator: left edge stripe
        if (sections_[i].lufs_integrated != 0.0f) {
            juce::Colour lufsCol = lufsColour(sections_[i].lufs_integrated);
            g.setColour(lufsCol.withAlpha(0.65f));
            g.fillRect(r.withWidth(3.0f).reduced(0.0f, 2.0f));
        }

        // Section label (top portion) + LUFS reading (bottom)
        g.setColour(KhensuTheme::textSecondary);
        if (sections_[i].lufs_integrated != 0.0f) {
            auto labelR = r.withHeight(r.getHeight() * 0.55f);
            auto lufsR = r.withTrimmedTop(r.getHeight() * 0.55f);
            g.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::plain));
            g.drawText(sections_[i].label, labelR.toNearestInt(), juce::Justification::centred, true);
            g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::plain));
            g.setColour(lufsColour(sections_[i].lufs_integrated).withAlpha(0.95f));
            g.drawText(juce::String(sections_[i].lufs_integrated, 1) + "L", lufsR.toNearestInt(),
                       juce::Justification::centred, true);
        } else {
            g.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::plain));
            g.drawText(sections_[i].label, r.toNearestInt(), juce::Justification::centred, true);
        }
    }

    // --- Waveform thumbnail (LUFS-tinted via Image Cache) ---
    if (waveformCacheNeedsRepaint_ || waveformCache_.getWidth() != b.getWidth() ||
        waveformCache_.getHeight() != b.getHeight()) {
        recreateWaveformCache(b);
    }
    g.drawImageAt(waveformCache_, b.getX(), b.getY());

    // --- Loudness mapping helpers ---
    auto lufsToY = [&](float lufs) -> float {
        // Map LUFS range [-36, -6] to [b.getBottom(), b.getY()]
        float ratio = (lufs - (-36.0f)) / (30.0f);
        ratio = juce::jlimit(0.0f, 1.0f, ratio);
        return (float)b.getBottom() - ratio * (float)b.getHeight();
    };

    // --- Shaded Target Envelope Corridor (e.g., -16 to -11 LUFS for streaming genre targets) ---
    {
        float targetTop = lufsToY(-11.0f);
        float targetBottom = lufsToY(-16.0f);
        g.setColour(khensu::KhensuTheme::textPrimary.withAlpha(0.05f));
        g.fillRect((float)b.getX(), targetTop, (float)b.getWidth(), targetBottom - targetTop);
        g.setColour(khensu::KhensuTheme::textPrimary.withAlpha(0.15f));
        g.drawHorizontalLine(juce::roundToInt(targetTop), (float)b.getX(), (float)b.getRight());
        g.drawHorizontalLine(juce::roundToInt(targetBottom), (float)b.getX(), (float)b.getRight());
    }

    // --- Excessive Limiting / Squashed dynamics highlights ---
    for (size_t i = 0; i < sections_.size(); ++i) {
        if (sections_[i].lufs_integrated != 0.0f) {
            const bool tooLoud = sections_[i].lufs_integrated > -11.0f;
            const bool tooSquashed = sections_[i].crest_db > 0.0f && sections_[i].crest_db < 6.5f;
            if (tooLoud || tooSquashed) {
                float rx0 = xFor(sections_[i].start_sec);
                float rx1 = (i + 1 < sections_.size()) ? xFor(sections_[i + 1].start_sec) : (float)getWidth();

                // Red hot limiting/squashed glow overlay
                g.setColour(KhensuTheme::accentRed.withAlpha(0.13f));
                g.fillRect((float)b.getX() + rx0, (float)b.getY(), rx1 - rx0, (float)b.getHeight());

                // Overlay text alert
                g.setColour(KhensuTheme::accentRed);
                g.setFont(khensu::KhensuTheme::uiFont(8.0f, juce::Font::bold));
                g.drawText(tooSquashed ? "SQUASHED" : "HOT LIMIT", (int)(rx0 + b.getX() + 4), (int)(b.getY() + 2),
                           (int)(rx1 - rx0 - 8), 10, juce::Justification::centred);
            }
        }
    }

    // --- Drag-selection Active Loops / Target Zones ---
    if (loopStartSec_ >= 0.0f && loopEndSec_ >= 0.0f) {
        float rx0 = xFor(std::min(loopStartSec_, loopEndSec_));
        float rx1 = xFor(std::max(loopStartSec_, loopEndSec_));
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.18f));
        g.fillRect((float)b.getX() + rx0, (float)b.getY(), rx1 - rx0, (float)b.getHeight());
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.6f));
        g.drawVerticalLine(juce::roundToInt(b.getX() + rx0), (float)b.getY(), (float)b.getBottom());
        g.drawVerticalLine(juce::roundToInt(b.getX() + rx1), (float)b.getY(), (float)b.getBottom());
    }

    // --- Continuous Loudness Path ---
    {
        juce::Path loudnessPath;
        bool firstPoint = true;
        for (size_t i = 0; i < sections_.size(); ++i) {
            if (sections_[i].lufs_integrated == 0.0f) continue;
            float start = sections_[i].start_sec;
            float end = (i + 1 < sections_.size()) ? sections_[i + 1].start_sec : total_;
            float mid = (start + end) / 2.0f;
            float px = xFor(mid);
            float py = lufsToY(sections_[i].lufs_integrated);

            if (firstPoint) {
                loudnessPath.startNewSubPath((float)b.getX() + px, py);
                firstPoint = false;
            } else {
                loudnessPath.lineTo((float)b.getX() + px, py);
            }
        }
        if (!loudnessPath.isEmpty()) {
            g.setColour(KhensuTheme::accentTeal); // Cyan neon LUFS line
            g.strokePath(loudnessPath, juce::PathStrokeType(2.0f));
        }
    }

    // --- Section division lines with white circular pin heads (#19 audit & reference style) ---
    for (size_t i = 0; i < sections_.size(); ++i) {
        float sx = xFor(sections_[i].start_sec);

        // Draw vertical division line in the waveform container
        g.setColour(khensu::KhensuTheme::textPrimary.withAlpha(0.45f));
        g.drawVerticalLine(juce::roundToInt(sx), (float)b.getY(), (float)b.getBottom());

        // Draw circular pin head
        g.setColour(khensu::KhensuTheme::textPrimary);
        g.fillEllipse(sx - 3.0f, (float)b.getY() - 3.0f, 6.0f, 6.0f);
    }

    // --- Playhead needle ---
    if (playheadSec_ >= 0.0f) {
        float px = xFor(playheadSec_);
        g.setColour(KhensuTheme::accentRed); // vibrant red
        g.fillRect((float)b.getX() + px - 1.0f, (float)b.getY(), 2.0f, (float)b.getHeight());

        // Draw a small triangular handle at the top of the playhead
        juce::Path head;
        head.addTriangle((float)b.getX() + px, (float)b.getY() + 6.0f, (float)b.getX() + px - 5.0f, (float)b.getY(),
                         (float)b.getX() + px + 5.0f, (float)b.getY());
        g.fillPath(head);
    }

    // --- Recommendation markers (#19 audit & live feedback) ---
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
        float tStart = getRecTime(rec);
        if (tStart < 0.0f) continue;

        juce::Colour markerCol = rec.severity >= 66.0f   ? KhensuTheme::accentRed
                                 : rec.severity >= 33.0f ? KhensuTheme::accentAmber
                                                         : KhensuTheme::accentTeal;

        // Draw shaded block if range is defined
        if (rec.time_range.has_value() && rec.time_range->end_sec > rec.time_range->start_sec) {
            float rx0 = xFor(rec.time_range->start_sec);
            float rx1 = xFor(rec.time_range->end_sec);
            g.setColour(markerCol.withAlpha(0.10f));
            g.fillRect((float)b.getX() + rx0, (float)b.getY(), rx1 - rx0, (float)b.getHeight());
        }

        // Draw warning flag icon
        float mx = xFor(tStart);
        float my = (float)b.getY() + 10.0f;
        g.setColour(markerCol);
        juce::Path p;
        p.addTriangle(mx, my - 6.0f, mx - 5.0f, my + 4.0f, mx + 5.0f, my + 4.0f);
        g.fillPath(p);

        // Draw tiny exclamation mark in the triangle
        g.setColour(juce::Colours::black); // theme-exempt(glyph on a saturated accent marker - black reads in every theme)
        g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::bold));
        g.drawText("!", (int)mx - 4, (int)my - 4, 8, 8, juce::Justification::centred);

        // Check if mouse is hovering close to this marker
        if (hoverTimeSec_ >= 0.0f && std::abs(hoverTimeSec_ - tStart) < 3.5f) {
            hoveredRec = &rec;
            hoveredRecTime = tStart;
        }
    }

    // --- Hook markers (strength-proportional teal bars) ---
    for (const auto& m : markers_) {
        const float x0 = xFor(m.start_sec);
        const float x1 = xFor(m.end_sec);
        const float h = juce::jlimit(0.0f, 1.0f, m.strength) * (float)b.getHeight();
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.60f));
        g.fillRect((float)b.getX() + x0, (float)b.getBottom() - h, juce::jmax(2.0f, x1 - x0), h);
    }

    // --- Beat ticks ---
    const float beatH = b.getHeight() * 0.25f;
    g.setColour(KhensuTheme::textSecondary.withAlpha(0.4f));
    for (float sec : beats_)
        g.fillRect((float)b.getX() + xFor(sec), (float)b.getBottom() - beatH, 1.0f, beatH);

    // --- Downbeat ticks ---
    const float downH = b.getHeight() * 0.45f;
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.65f));
    for (float sec : downbeats_)
        g.fillRect((float)b.getX() + xFor(sec) - 0.5f, (float)b.getBottom() - downH, 2.0f, downH);

    // --- Hover time readout & Tooltip popup ---
    if (hoverTimeSec_ >= 0.0f) {
        const float hx = xFor(hoverTimeSec_);

        // Draw hover playhead needle
        g.setColour(khensu::KhensuTheme::textPrimary.withAlpha(0.35f));
        g.drawVerticalLine(juce::roundToInt(hx), (float)getLocalBounds().getY(), (float)getLocalBounds().getBottom());

        if (hoveredRec != nullptr) {
            // Render active recommendation tooltip
            juce::Colour mCol = hoveredRec->severity >= 66.0f   ? KhensuTheme::accentRed
                                : hoveredRec->severity >= 33.0f ? KhensuTheme::accentAmber
                                                                : KhensuTheme::accentTeal;
            g.setFont(khensu::KhensuTheme::uiFont(11.0f, juce::Font::bold));
            juce::String tooltipText = hoveredRec->what;
            const int textW = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), tooltipText) + 16;
            int textH = 22;
            int tx = (int)xFor(hoveredRecTime) - textW / 2;
            tx = juce::jlimit(getLocalBounds().getX() + 4, getLocalBounds().getRight() - textW - 4, tx);
            int ty = (int)b.getY() + 20;

            g.setColour(KhensuTheme::bgVoid.withAlpha(0.95f));
            g.fillRoundedRectangle((float)tx, (float)ty, (float)textW, (float)textH, 4.0f);
            g.setColour(mCol);
            g.drawRoundedRectangle((float)tx, (float)ty, (float)textW, (float)textH, 4.0f, 1.5f);
            g.setColour(KhensuTheme::textPrimary);
            g.drawText(tooltipText, tx, ty, textW, textH, juce::Justification::centred);
        } else {
            // Find section hover metrics
            float sectionCrest = 0.0f;
            float sectionLufs = 0.0f;
            juce::String sectionLabel = "";
            for (size_t i = 0; i < sections_.size(); ++i) {
                const float nextStart = (i + 1 < sections_.size()) ? sections_[i + 1].start_sec : total_;
                if (hoverTimeSec_ >= sections_[i].start_sec && hoverTimeSec_ < nextStart) {
                    sectionLufs = sections_[i].lufs_integrated;
                    sectionCrest = sections_[i].crest_db;
                    sectionLabel = sections_[i].label;
                    break;
                }
            }

            juce::String hoverText;
            if (!sectionLabel.isEmpty() && sectionLufs != 0.0f) {
                float targetDiff = sectionLufs - (-14.0f); // Default streaming target
                hoverText = sectionLabel + " | " + juce::String(sectionLufs, 1) +
                            " LUFS | Crest: " + juce::String(sectionCrest, 1) +
                            " dB (Dev: " + (targetDiff >= 0.0f ? "+" : "") + juce::String(targetDiff, 1) + " LU)";
            } else {
                const int min = (int)hoverTimeSec_ / 60;
                const int sec = (int)hoverTimeSec_ % 60;
                hoverText = juce::String(min) + ":" + juce::String(sec).paddedLeft('0', 2);
            }

            g.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::plain));
            const int textW = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), hoverText) + 16;
            int textH = 18;
            int tx = (int)hx - textW / 2;
            tx = juce::jlimit(getLocalBounds().getX() + 4, getLocalBounds().getRight() - textW - 4, tx);
            int ty = 2;

            g.setColour(KhensuTheme::bgSurface.darker(0.25f).withAlpha(0.92f));
            g.fillRoundedRectangle((float)tx, (float)ty, (float)textW, (float)textH, 3.0f);
            g.setColour(khensu::KhensuTheme::textPrimary.withAlpha(0.9f));
            g.drawText(hoverText, tx, ty, textW, textH, juce::Justification::centred, false);
        }
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

void TimelineComponent::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (total_ <= 0.0f) return;
    const float clickTimeSec = timeForX((float)e.x);

    // Fire the seek callback if set (preferred path - no blocking dialog)
    if (seekCallback_) {
        seekCallback_(clickTimeSec);
        return;
    }

    // Fallback: show informational dialog (original behaviour)
    juce::String sectionLabel = "Unknown";
    for (size_t i = 0; i < sections_.size(); ++i) {
        const float nextStart = (i + 1 < sections_.size()) ? sections_[i + 1].start_sec : total_;
        if (clickTimeSec >= sections_[i].start_sec && clickTimeSec < nextStart) {
            sectionLabel = sections_[i].label;
            break;
        }
    }

    const int min = (int)clickTimeSec / 60;
    const int sec = (int)clickTimeSec % 60;
    juce::String timeStr = juce::String(min) + ":" + juce::String(sec).paddedLeft('0', 2);

    juce::NativeMessageBox::showMessageBoxAsync(
        juce::MessageBoxIconType::InfoIcon, "DAW Seeker Triggered",
        "Seeking DAW playback to position: " + timeStr +
            "\n"
            "Active Section: " +
            sectionLabel +
            "\n\n"
            "[DAW transport synchronization is dispatched via the Khensu Copilot VST3/AU plugin]");
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

    // If the selection is tiny, cancel loop selection
    if (loopEndSec_ - loopStartSec_ < 0.2f) {
        loopStartSec_ = -1.0f;
        loopEndSec_ = -1.0f;
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
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.40f));
        thumbnail_.drawChannels(g, b.reduced(1, 1), 0.0, thumbnail_.getTotalLength(), 1.0f);

        g.setColour(KhensuTheme::accentTeal.withAlpha(0.85f));
        thumbnail_.drawChannel(g, b.reduced(1, 1), 0.0, thumbnail_.getTotalLength(), 0, 0.95f);
    } else {
        g.setColour(KhensuTheme::bgSurface.brighter(0.06f));
        g.fillRect(b);
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.35f));
        g.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::italic));
        g.drawText("No audio file loaded", b, juce::Justification::centred, false);
    }

    waveformCacheNeedsRepaint_ = false;
}

} // namespace khensu
