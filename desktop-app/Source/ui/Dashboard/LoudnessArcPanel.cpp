// desktop-app/Source/ui/Dashboard/LoudnessArcPanel.cpp
#include "LoudnessArcPanel.h"
#include <khensu/session/Session.h>
#include <khensu/targets/GenreDatabase.h>
#include "../shared/KhensuLookAndFeel.h"
#include <algorithm>
#include <cmath>
#include "../shared/KhensuTheme.h"

namespace khensu {

LoudnessArcPanel::LoudnessArcPanel(IAppController& c) : PanelCard(c, "Loudness Arc")
{
    setExplainer(TRANS("Loudness Arc"),
                 TRANS("Per-section integrated LUFS displayed as a horizontal bar chart. Shows the loudness "
                 "journey across your arrangement - chorus lift, verse headroom, and bridge dynamics. "
                 "The shaded band is the genre's acceptable LUFS range; the dashed line is its target. "
                 "Green = loud, amber = moderate, red = quiet relative to the range.\n\n"
                 "The footer scores your macrodynamics: the measured verse->chorus lift against what "
                 "this genre typically delivers. A flat arc usually means bus over-compression has "
                 "squashed the contrast your arrangement wrote - the chorus stops feeling like a lift "
                 "even though the parts changed."),
                 "integrated_loudness");
}

void LoudnessArcPanel::setData(const AnalysisResult& r)
{
    bars_.clear();
    bool hasSecAnalysis = r.section_analysis.has_value() && !r.section_analysis->sections.empty();

    SectionAnalysis sa;
    if (hasSecAnalysis) {
        sa = *r.section_analysis;
    } else {
        sa.sections = {{"Intro", "intro", "A", 0.0f, 30.0f, 30.0f, -22.4f, 12.0f, 0.0f, -90.0f, -1.0f, -1.0f, -1.0f, 0.8f},
                       {"Verse 1", "verse", "B", 30.0f, 90.0f, 60.0f, -18.2f, 12.0f, 0.0f, -90.0f, -1.0f, -1.0f, -1.0f, 0.8f},
                       {"Chorus 1", "chorus", "C", 90.0f, 150.0f, 60.0f, -14.1f, 12.0f, 0.0f, -90.0f, -1.0f, -1.0f, -1.0f, 0.8f},
                       {"Verse 2", "verse", "B", 150.0f, 210.0f, 60.0f, -17.8f, 12.0f, 0.0f, -90.0f, -1.0f, -1.0f, -1.0f, 0.8f},
                       {"Chorus 2", "chorus", "C", 210.0f, 270.0f, 60.0f, -13.9f, 12.0f, 0.0f, -90.0f, -1.0f, -1.0f, -1.0f, 0.8f},
                       {"Bridge", "bridge", "D", 270.0f, 300.0f, 30.0f, -16.5f, 12.0f, 0.0f, -90.0f, -1.0f, -1.0f, -1.0f, 0.8f},
                       {"Chorus 3", "chorus", "C", 300.0f, 360.0f, 60.0f, -13.5f, 12.0f, 0.0f, -90.0f, -1.0f, -1.0f, -1.0f, 0.8f},
                       {"Outro", "outro", "E", 360.0f, 390.0f, 30.0f, -21.0f, 12.0f, 0.0f, -90.0f, -1.0f, -1.0f, -1.0f, 0.8f}};
    }

    float lo = 0.0f, hi = -99.0f;
    bool anyValid = false;

    for (const auto& s : sa.sections) {
        SectionBar b;
        b.label = juce::String(s.label);
        b.lufs = s.lufs_integrated;
        b.valid = (s.lufs_integrated < -0.5f); // 0.0f = not computed
        if (b.valid) {
            if (!anyValid) {
                lo = s.lufs_integrated;
                hi = s.lufs_integrated;
                anyValid = true;
            }
            lo = juce::jmin(lo, s.lufs_integrated);
            hi = juce::jmax(hi, s.lufs_integrated);
        }
        bars_.push_back(b);
    }

    if (anyValid) {
        minLufs_ = lo - 2.0f; // a little below the quietest for visual breathing room
        maxLufs_ = hi + 1.0f;
    }

    if (auto* session = controller_.getActiveSession()) {
        if (auto genre = controller_.genreDatabase().findById(session->target.primary_genre)) {
            minLufs_ = juce::jmin(minLufs_, genre->lufs_range_min - 1.5f);
            maxLufs_ = juce::jmax(maxLufs_, genre->lufs_range_max + 1.5f);
        }
    }

    computeContrast();
    repaint();
}

void LoudnessArcPanel::computeContrast()
{
    contrastValid_ = false;

    // Lift sections vs. base sections: real role names when present, else the
    // roles inferred from the detector's repeated A/B/C clusters, else the
    // loudest section vs. the median (approximate) - see SectionContrast.h.
    std::vector<section_contrast::Bar> measured;
    for (const auto& bar : bars_)
        if (bar.valid) measured.push_back({bar.label, bar.lufs});
    contrast_ = section_contrast::compute(measured);
    if (contrast_.basis == section_contrast::Basis::None) return; // too few sections for a meaningful arc
    measuredContrastDb_ = contrast_.liftDb;

    // Map the genre's 0-1 chorus_contrast_norm onto a dB expectation. 0.0 ->
    // ~1 dB (through-composed / ambient: barely any lift), 0.5 -> ~5 dB
    // (typical pop lift), 1.0 -> ~9 dB (EDM drop-style contrast).
    expectedContrastDb_ = 1.0f;
    if (auto* session = controller_.getActiveSession())
        if (auto genre = controller_.genreDatabase().findById(session->target.primary_genre))
            expectedContrastDb_ = 1.0f + genre->chorus_contrast_norm * 8.0f;

    contrastValid_ = true;
}

void LoudnessArcPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    auto b = bodyArea().withTrimmedTop(12); // Leave room for target line header label

    // Macrodynamics contrast footer (drawn last, but carved out first so the
    // bars don't paint over it).
    juce::Rectangle<int> contrastRow;
    if (contrastValid_) contrastRow = b.removeFromBottom(16);

    if (bars_.empty()) {
        drawEmptyState(g, "No loudness arc yet", "Run a full analysis to see the per-section loudness arc.");
        return;
    }

    const int n = (int)bars_.size();
    const float range = maxLufs_ - minLufs_;
    const float barH = juce::jmax(4.0f, (float)b.getHeight() / (float)n - 3.0f);
    const float labelW = 74.0f;
    const float valW = 46.0f;
    const float barAreaW = (float)b.getWidth() - labelW - valW;

    // Genre envelope: shade the acceptable LUFS range (lufs_range_min/max)
    // behind the bars, so it reads as a band the whole arc should sit
    // within - not just a single target line with no sense of tolerance.
    if (range > 0.01f) {
        if (auto* session = controller_.getActiveSession()) {
            if (auto genre = controller_.genreDatabase().findById(session->target.primary_genre)) {
                const float loNorm = juce::jlimit(0.0f, 1.0f, (genre->lufs_range_min - minLufs_) / range);
                const float hiNorm = juce::jlimit(0.0f, 1.0f, (genre->lufs_range_max - minLufs_) / range);
                if (hiNorm > loNorm) {
                    const float bandX = (float)b.getX() + labelW + barAreaW * loNorm;
                    const float bandW = barAreaW * (hiNorm - loNorm);
                    g.setColour(KhensuTheme::accentTeal.withAlpha(0.06f));
                    g.fillRect(bandX, (float)b.getY(), bandW, (float)b.getHeight());
                }
            }
        }
    }

    float y = (float)b.getY();

    for (const auto& bar : bars_) {
        juce::Rectangle<float> row((float)b.getX(), y, (float)b.getWidth(), barH);

        // Section label
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(khensu::KhensuTheme::uiFont(9.5f, juce::Font::plain));
        auto labelRect = row.removeFromLeft(labelW);
        g.drawText(bar.label, labelRect.toNearestInt(), juce::Justification::centredLeft);

        if (bar.valid) {
            const float norm = range > 0.01f ? juce::jlimit(0.0f, 1.0f, (bar.lufs - minLufs_) / range) : 0.5f;

            // Colour: teal accent for loudest zone, amber middle, muted red for quiet
            juce::Colour barCol = norm > 0.65f   ? KhensuTheme::accentTeal
                                  : norm > 0.35f ? KhensuTheme::accentAmber
                                                 : KhensuTheme::accentRed;

            // Background track
            g.setColour(KhensuTheme::bgRaised);
            g.fillRoundedRectangle(row.getX(), row.getY(), barAreaW, barH, 2.0f);

            // Fill bar - proportional to normalised LUFS
            g.setColour(barCol.withAlpha(0.80f));
            g.fillRoundedRectangle(row.getX(), row.getY(), barAreaW * norm, barH, 2.0f);

            // LUFS value label
            g.setColour(KhensuTheme::textPrimary);
            g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::bold));
            juce::Rectangle<float> valRect(row.getX() + barAreaW + 4.0f, row.getY(), valW, barH);
            g.drawText(juce::String(bar.lufs, 1) + " L", valRect.toNearestInt(), juce::Justification::centredLeft);

        } else {
            // Not computed marker
            g.setColour(KhensuTheme::textSecondary.withAlpha(0.4f));
            g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::plain));
            g.drawText("-", row.toNearestInt(), juce::Justification::centredLeft);
        }

        y += barH + 3.0f;
        if (y + barH > (float)b.getBottom()) break;
    }

    // Paint the target LUFS reference vertical line
    float targetLufs = -99.0f;
    if (auto* session = controller_.getActiveSession()) {
        if (auto genre = controller_.genreDatabase().findById(session->target.primary_genre)) {
            targetLufs = genre->lufs_target;
        }
    }
    if (targetLufs > minLufs_ && targetLufs < maxLufs_ && range > 0.01f) {
        const float normTarget = (targetLufs - minLufs_) / range;
        const float tx = b.getX() + labelW + barAreaW * normTarget;

        g.setColour(KhensuTheme::accentTeal.withAlpha(0.5f));
        float dashes[] = {3.0f, 3.0f};
        g.drawDashedLine({tx, (float)b.getY(), tx, (float)b.getBottom()}, dashes, 2, 1.0f);

        g.setFont(khensu::KhensuTheme::uiFont(7.5f, juce::Font::bold));
        g.setColour(KhensuTheme::accentTeal);
        g.drawText("TARGET (" + juce::String(juce::roundToInt(targetLufs)) + " LUFS)", (int)tx - 45, (int)b.getY() - 11, 90, 10,
                   juce::Justification::centred);
    }

    // ---- Macrodynamics contrast footer ----
    if (contrastValid_ && !contrastRow.isEmpty()) {
        const float delta = measuredContrastDb_ - expectedContrastDb_;
        // Within ±2 dB of the genre expectation = healthy; short = flattened
        // (the over-compression signature); far over = whiplash contrast.
        juce::Colour col = std::abs(delta) <= 2.0f ? KhensuTheme::accentTeal
                           : delta < 0.0f          ? KhensuTheme::accentRed
                                                   : KhensuTheme::accentAmber;
        juce::String verdict = std::abs(delta) <= 2.0f ? "healthy contrast"
                               : delta < 0.0f          ? "flattened - check bus compression/limiting"
                                                       : "extreme - verses may feel disconnected";

        juce::String text;
        using section_contrast::Basis;
        if (contrast_.basis == Basis::RoleLabels)
            text << "VERSE->CHORUS LIFT: ";
        else if (contrast_.basis == Basis::RepeatStructure)
            // e.g. "REPEATED-SECTION LIFT (B over A, inferred): " - roles come
            // from which repeating section is loudest, not from named parts.
            text << TRANS("REPEATED-SECTION LIFT") << " (" << contrast_.liftLabel << " " << TRANS("over") << " "
                 << contrast_.baseLabel << ", " << TRANS("inferred") << "): ";
        else
            text << "PEAK-SECTION LIFT (approx): ";
        text << juce::String(measuredContrastDb_, 1) << " dB  -  genre expects ~"
             << juce::String(expectedContrastDb_, 1) << " dB  -  " << verdict;

        g.setFont(khensu::KhensuTheme::uiFont(8.5f, juce::Font::bold));
        g.setColour(col);
        g.drawText(text, contrastRow, juce::Justification::centredLeft, true);
    }
}

void LoudnessArcPanel::resized() { PanelCard::resized(); }

} // namespace khensu
