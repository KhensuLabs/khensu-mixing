#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>
#include "KhensuTheme.h"

namespace khensu {

/// The five-chip mix-health strip under the header (loudness, dynamics,
/// spectral, stereo, artifacts). Clicking a chip opens its reasoning trace.
class MixHealthRollupStrip final : public juce::Component {
  public:
    MixHealthRollupStrip()
    {
        for (int i = 0; i < 5; ++i) {
            chips_[i].setJustificationType(juce::Justification::centred);
            chips_[i].setFont(juce::Font(juce::FontOptions{10.5f, juce::Font::bold}));
            chips_[i].setInterceptsMouseClicks(false, false); // clicks fall through to the strip
            addAndMakeVisible(chips_[i]);
        }
        setEmpty();
    }

    /// Fired with the chip index (0..4) when a metric chip is clicked - the
    /// host opens the reasoning trace for that metric ("why this?").
    std::function<void(int)> onChipClicked;

    void mouseDown(const juce::MouseEvent& e) override
    {
        for (int i = 0; i < 5; ++i)
            if (chips_[i].getBounds().contains(e.getPosition())) {
                if (onChipClicked) onChipClicked(i);
                return;
            }
    }
    juce::MouseCursor getMouseCursor() override { return juce::MouseCursor::PointingHandCursor; }

    void paint(juce::Graphics& g) override
    {
        // Draw overall glass background. Theme tokens, not literals - the fixed
        // black wash and white rule were invisible on the Light theme (UI-01).
        g.setColour(KhensuTheme::bgVoid.withAlpha(0.3f));
        g.fillAll();
        g.setColour(KhensuTheme::borderActive);
        g.drawHorizontalLine(getHeight() - 1, 0.0f, (float)getWidth());

        // Draw pill backgrounds
        for (int i = 0; i < 5; ++i) {
            if (chips_[i].isVisible()) {
                auto b = chips_[i].getBounds().toFloat();
                g.setColour(chips_[i].findColour(juce::Label::backgroundColourId));
                g.fillRoundedRectangle(b, 4.0f);
            }
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(6, 2);
        int w = r.getWidth() / 5;
        for (int i = 0; i < 5; ++i) {
            chips_[i].setBounds(r.removeFromLeft(w).reduced(4, 0));
        }
    }

    /// `lufsLo`/`lufsHi`: the session genre's LUFS window (§10.3 genre-aware
    /// thresholds); the generic -18..-9 without a genre.
    void updateData(const AnalysisResult& r, float lufsLo = -18.0f, float lufsHi = -9.0f)
    {
        // 1. Loudness
        float lufs = r.loudness.lufs_integrated;
        juce::String lufsText = TRANS("Loudness:") + " " + juce::String(lufs, 1) + " LUFS";
        juce::Colour lufsCol = (lufs >= lufsLo && lufs <= lufsHi) ? KhensuTheme::accentGreen : KhensuTheme::accentAmber;
        chips_[0].setText(lufsText, juce::dontSendNotification);
        chips_[0].setColour(juce::Label::backgroundColourId, lufsCol.withAlpha(0.12f));
        chips_[0].setColour(juce::Label::textColourId, lufsCol);

        // 2. Dynamics
        float crest = r.dynamics.crest_factor_db;
        juce::String crestText = TRANS("Dynamics:") + " " + juce::String(crest, 1) + " dB";
        juce::Colour crestCol = (crest >= 6.0f && crest <= 18.0f) ? KhensuTheme::accentGreen : KhensuTheme::accentRed;
        chips_[1].setText(crestText, juce::dontSendNotification);
        chips_[1].setColour(juce::Label::backgroundColourId, crestCol.withAlpha(0.12f));
        chips_[1].setColour(juce::Label::textColourId, crestCol);

        // 3. Spectral
        float spec = r.spectral.balance_mid;
        juce::String specText = TRANS("Spectral Mid:") + " " + juce::String(juce::roundToInt(spec * 100.0f)) + "%";
        juce::Colour specCol = (spec >= 0.25f && spec <= 0.55f) ? KhensuTheme::accentGreen : KhensuTheme::accentAmber;
        chips_[2].setText(specText, juce::dontSendNotification);
        chips_[2].setColour(juce::Label::backgroundColourId, specCol.withAlpha(0.12f));
        chips_[2].setColour(juce::Label::textColourId, specCol);

        // 4. Stereo
        float corr = r.stereo.correlation;
        juce::String corrText = TRANS("Stereo Corr:") + " " + juce::String(corr, 2);
        juce::Colour corrCol = (corr >= 0.0f) ? KhensuTheme::accentGreen : KhensuTheme::accentRed;
        chips_[3].setText(corrText, juce::dontSendNotification);
        chips_[3].setColour(juce::Label::backgroundColourId, corrCol.withAlpha(0.12f));
        chips_[3].setColour(juce::Label::textColourId, corrCol);

        // 5. Artifacts
        int clips = r.artifacts.clipping_frame_count;
        juce::String artText = TRANS("Clips:") + " " + juce::String(clips);
        juce::Colour artCol = (clips == 0) ? KhensuTheme::accentGreen : KhensuTheme::accentRed;
        chips_[4].setText(artText, juce::dontSendNotification);
        chips_[4].setColour(juce::Label::backgroundColourId, artCol.withAlpha(0.12f));
        chips_[4].setColour(juce::Label::textColourId, artCol);

        repaint();
    }

    void setEmpty()
    {
        chips_[0].setText(TRANS("Loudness: --"), juce::dontSendNotification);
        chips_[1].setText(TRANS("Dynamics: --"), juce::dontSendNotification);
        chips_[2].setText(TRANS("Spectral: --"), juce::dontSendNotification);
        chips_[3].setText(TRANS("Stereo: --"), juce::dontSendNotification);
        chips_[4].setText(TRANS("Artifacts: --"), juce::dontSendNotification);
        for (int i = 0; i < 5; ++i) {
            chips_[i].setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
            // textTertiary, not a faded white - the latter vanished against the
            // Light theme's pale strip (UI-01).
            chips_[i].setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
        }
        repaint();
    }

  private:
    juce::Label chips_[5];

    // The populated chip colours are DERIVED from the current analysis (each
    // chip takes its status colour from the measurement it shows), so they are
    // re-resolved by setData()/setEmpty() rather than from a theme handler -
    // there is nothing to re-apply without the data. A theme switch therefore
    // leaves a populated strip on the old palette until the next analysis push;
    // setEmpty() above is the one path that can be made theme-correct on its own.
};

} // namespace khensu
