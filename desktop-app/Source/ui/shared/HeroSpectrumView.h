// desktop-app/Source/ui/shared/HeroSpectrumView.h
// UI-1: the Issues screen's hero - the track's long-term spectrum against the
// genre target and its tolerance, with every finding drawn where it lives:
// frequency findings on the spectrum (bands, the tilt as a sloped line,
// resonances as markers), the rest as gauges and chips in a strip beneath
// (loudness against the genre window, true peak, LRA, crest, width, DC...).
// Hovering names a finding; clicking selects it (the queue scrolls to its
// row); selecting a row highlights its mark. A fix that lands morphs the
// curve from before to the expected after (~250 ms) and the fix check's
// result shrinks and fades the finding's mark by the measured change.
// The data is hero_spectrum::Model (HeroSpectrumModel.h, unit-tested).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "HeroSpectrumModel.h"
#include "KhensuTheme.h" // ThemeListener

namespace khensu {

class HeroSpectrumView final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer {
  public:
    HeroSpectrumView();
    ~HeroSpectrumView() override;

    /// The spectrum, target and findings. `afterResponse` (one value per
    /// model.spectrum point, empty = none) is the level change of the fixes
    /// the current version is built on. `animate`: morph from what is shown
    /// (the user watched the change), else snap (a screen just opened).
    void setModel(hero_spectrum::Model model, std::vector<float> afterResponse, bool animate);
    /// The selected issue's fix at the chosen amount, before it renders
    /// (OVH-6); empty = none.
    void setPreviewResponse(std::vector<float> response);
    /// VIZ-3c: asked every frame whether the original (A) is what plays; the
    /// curve then glides between the current version and the original
    /// instead of snapping when A/B is toggled (from any control).
    void setOriginalProbe(std::function<bool()> probe) { originalProbe_ = std::move(probe); }
    bool showingOriginal() const noexcept { return abTo_ < 0.5f; }
    /// Highlights the finding with this key (Finding::key()); "" = none.
    void setSelected(const std::string& key);
    const std::string& selected() const noexcept { return selected_; }

    /// A finding was clicked (its key).
    std::function<void(const std::string& key)> onFindingClicked;
    /// VIZ-P2: a measurement tile was clicked (hero_spectrum::GaugeIndex).
    std::function<void(int gauge)> onMetricClicked;

    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

    /// For tests: the finding under a point ("" = none), and the layout.
    std::string findingAt(juce::Point<float> pos) const;
    juce::Rectangle<float> plotArea() const noexcept;
    juce::Rectangle<float> stripArea() const noexcept;   ///< tiles + findings
    juce::Rectangle<float> tilesArea() const noexcept;   ///< VIZ-P2: the measurements
    juce::Rectangle<float> findingsArea() const noexcept; ///< VIZ-U1: "Top findings"
    /// The tile under a point (GaugeIndex), or -1.
    int tileAt(juce::Point<float> pos) const;
    float xForHz(float hz) const noexcept;
    float yForDb(float db) const noexcept;
    bool isAnimating() const noexcept { return isTimerRunning(); }

    static constexpr int kTilesH = 82;
    static constexpr int kFindingsH = 34;
    static constexpr int kStripH = kTilesH + 8 + kFindingsH;

  private:
    void timerCallback() override;
    float eased() const noexcept;
    float abWeight() const noexcept; ///< 1 = the current version's curve, 0 = the original's
    void onVBlank();
    float shownAfter(size_t i) const noexcept;
    float shownRemaining(const std::string& key) const noexcept;
    void updateDbRange();

    struct ChipCell {
        juce::Rectangle<float> bounds;
        int finding = -1;    ///< index into model_.findings, else -1
        int hiddenCount = 0; ///< "+N" overflow chip
    };
    /// The "Top findings" row: every non-frequency finding, open and worst first.
    std::vector<ChipCell> chipCells() const;
    std::vector<juce::Rectangle<float>> tileCells() const;

    void paintGrid(juce::Graphics& g, juce::Rectangle<float> plot) const;
    void paintTarget(juce::Graphics& g, juce::Rectangle<float> plot) const;
    void paintSpectrum(juce::Graphics& g, juce::Rectangle<float> plot) const;
    void paintFindings(juce::Graphics& g, juce::Rectangle<float> plot) const;
    void paintStrip(juce::Graphics& g) const;
    void paintTiles(juce::Graphics& g) const;
    void paintSpread(juce::Graphics& g, juce::Rectangle<float> plot) const;
    void paintReference(juce::Graphics& g, juce::Rectangle<float> plot) const;
    void paintCrosshair(juce::Graphics& g) const;
    void paintHeader(juce::Graphics& g) const;
    void paintHover(juce::Graphics& g) const;
    juce::Colour colourFor(const hero_spectrum::Finding& f) const;
    /// offsets: dB per spectrum point, scaled by amount; nullptr draws the original curve.
    juce::Path curvePath(juce::Rectangle<float> plot, const std::vector<float>* offsets, float amount) const;

    hero_spectrum::Model model_;
    std::vector<float> afterFrom_, afterTo_;
    std::map<std::string, float> remainingFrom_, remainingTo_;
    std::vector<float> preview_;
    double animStartMs_ = 0.0;
    static constexpr double kAnimMs = 250.0;
    float dbTop_ = 12.0f, dbBottom_ = -36.0f;
    std::string selected_;
    std::function<bool()> originalProbe_;
    float abFrom_ = 1.0f, abTo_ = 1.0f;
    double abStartMs_ = 0.0;
    std::unique_ptr<juce::VBlankAttachment> vblank_;
    std::string hovered_;
    juce::Point<float> hoverPos_;
    bool hoverInPlot_ = false;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] { repaint(); }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HeroSpectrumView)
};

} // namespace khensu
