// desktop-app/Source/ui/Dashboard/SpectralPanel.h
#pragma once
#include "../shared/PanelCard.h"
#include "../shared/HeatSpectrumComponent.h"
#include "../shared/CompareDeltaChip.h"
#include <khensu/analysis/AnalysisResult.h>
#include "../shared/KhensuTheme.h"

namespace khensu {

class SectionSpectralBar final : public juce::Component {
  public:
    SectionSpectralBar() = default;

    void setSections(const std::vector<float>& harshnessVals)
    {
        values_ = harshnessVals;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        if (values_.empty()) return;

        // Use custom font helper
        g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::plain));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText("HARSHNESS EVOLUTION (BY SECTION)", getLocalBounds().withHeight(12),
                   juce::Justification::centredLeft, false);

        auto b = getLocalBounds().withTrimmedTop(13).toFloat();
        if (b.getHeight() < 4.0f) return;

        const int n = (int)values_.size();
        const float spacing = 2.0f;
        const float barW = (b.getWidth() - spacing * (n - 1)) / (float)n;

        for (int i = 0; i < n; ++i) {
            const float t = juce::jlimit(0.0f, 1.0f, values_[i]);
            const float bx = b.getX() + (float)i * (barW + spacing);
            const float bh = b.getHeight() * (0.2f + 0.8f * t);
            const float by = b.getBottom() - bh;

            const bool harsh = values_[i] > 0.35f;
            auto col = harsh ? KhensuTheme::accentAmber : KhensuTheme::accentTeal;
            g.setColour(col.withAlpha(0.55f + 0.45f * t));
            g.fillRoundedRectangle(bx, by, barW, bh, 1.5f);
        }
    }

  private:
    std::vector<float> values_;
};

class SpectralPanel final : public PanelCard {
  public:
    explicit SpectralPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void resized() override;
    void setCompareModeEnabled(bool enabled);

  private:
    HeatSpectrumComponent spectrum_;
    juce::Label balance_;
    SectionSpectralBar sectionSpectral_;

    CompareDeltaChip compareChip_;
    bool compareEnabled_ = false;
    std::optional<ReferenceDeltas> lastRefDeltas_;
    void updateCompareChip();
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectralPanel)
};

} // namespace khensu
