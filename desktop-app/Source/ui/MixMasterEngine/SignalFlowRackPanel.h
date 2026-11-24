// desktop-app/Source/ui/MixMasterEngine/SignalFlowRackPanel.h
// The plugin chain's one desktop editor (WF-9) and, since UX2-2, the hero of
// Mix / Master: the built-in corrective DSP chain as a row of modules in
// signal order (high-pass, low-pass, three EQ bands / compressor, limiter,
// output), each with a power switch and KhensuKnob controls. Edits post to
// the chosen plugin instance (PLG-04); a chain another surface staged shows
// here to apply or discard.
#pragma once
#include "../shared/HoverCells.h"
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/ListenerHandle.h"
#include "../shared/PanelCard.h"
#include "../shared/PluginTargetChooser.h"
#include <khensu/ui/KhensuKnob.h>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace khensu {

class IAppController;

namespace rack_layout {

/// One module's footprint: how many knob columns it needs.
struct ModuleSpec {
    int slots = 1;
    int row = 0;
};

/// Width a row of `modules` needs at `knobW`; knobWidthFor() is the knob
/// column width that fits every row into `width`, clamped to [minKnob,
/// maxKnob] (below minKnob the rack scrolls sideways).

inline int rowWidth(const std::vector<ModuleSpec>& modules, int row, int knobW, int modulePad, int moduleGap, int knobGap)
{
    int w = 0, n = 0;
    for (const auto& m : modules) {
        if (m.row != row) continue;
        w += m.slots * knobW + (m.slots - 1) * knobGap + 2 * modulePad;
        ++n;
    }
    return n > 0 ? w + (n - 1) * moduleGap : 0;
}

inline int knobWidthFor(const std::vector<ModuleSpec>& modules, int width, int modulePad, int moduleGap, int knobGap,
                 int minKnob, int maxKnob)
{
    int best = maxKnob;
    int rows = 0;
    for (const auto& m : modules)
        rows = juce::jmax(rows, m.row + 1);
    for (int row = 0; row < rows; ++row) {
        int slots = 0, fixed = 0, n = 0;
        for (const auto& m : modules) {
            if (m.row != row) continue;
            slots += m.slots;
            fixed += (m.slots - 1) * knobGap + 2 * modulePad;
            ++n;
        }
        if (slots == 0) continue;
        fixed += (n - 1) * moduleGap;
        best = juce::jmin(best, (width - fixed) / slots);
    }
    return juce::jlimit(minKnob, maxKnob, best);
}

} // namespace rack_layout

class SignalFlowRackPanel final : public PanelCard, private juce::Timer {
  public:
    explicit SignalFlowRackPanel(IAppController& c);
    ~SignalFlowRackPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

    /// Height the rack wants at `width` (both module rows, no scrolling).
    int preferredHeight(int width) const;

    /// Tests: the knob / switch for a DSP parameter id.
    juce::Slider* knobFor(const juce::String& paramId) const;
    juce::Button* switchFor(const juce::String& paramId) const;
    const std::map<juce::String, float>& currentParams() const noexcept { return currentParams_; }

  private:
    void timerCallback() override; // polls the plugin's parameters
    void loadParamsFromServer();
    void pushParamsToServer();
    void applyParams(const std::map<juce::String, float>& params);

    enum class Unit { Hz, Db, Ms, Ratio, Q, Percent };

    struct Knob {
        juce::String paramId;
        std::unique_ptr<ui::KhensuKnob> knob;
        int module = 0;
        bool bandOnly = false; ///< the compressor's multiband controls
    };

    struct Switch {
        juce::String paramId;
        std::unique_ptr<juce::ToggleButton> button;
        int module = 0;
        bool power = true; ///< the module's on switch (vs. Multiband / Bypass)
    };

    struct Module {
        juce::String title;
        juce::String onParam; ///< "" = always on (Output)
        rack_layout::ModuleSpec spec;
        juce::Rectangle<int> bounds; ///< in canvas coordinates
    };

    int addModule(const juce::String& title, const char* onParam, int slots, int row);
    void addKnob(int module, const char* paramId, const juce::String& label, float min, float max, float defVal,
                 Unit unit, bool bandOnly = false);
    void addSwitch(int module, const char* paramId, const juce::String& label, bool power);
    void updateModuleStates();
    bool moduleOn(int module) const;
    void layoutCanvas(int width);
    void paintCanvas(juce::Graphics& g);
    /// VIZ-6a: the module's response / transfer curve / reduction history.
    void paintModuleViz(juce::Graphics& g, int module, juce::Rectangle<float> r, bool on);
    float paramValue(const char* id, float fallback) const;
    bool timelineRequested_ = false;
    bool isUserInteracting() const;

    struct Canvas final : public juce::Component, public juce::SettableTooltipClient {
        std::function<void(juce::Graphics&)> painter;
        HoverCells hover; ///< VIZ-U3: each module drawing's readout
        void paint(juce::Graphics& g) override
        {
            hover.clear();
            if (painter) painter(g);
        }
        void mouseMove(const juce::MouseEvent& e) override { hover.update(*this, e.position); }
        void mouseExit(const juce::MouseEvent&) override { setTooltip({}); }
    };

    juce::Viewport viewport_;
    Canvas canvas_;

    std::vector<Module> modules_;
    std::vector<Knob> knobs_;
    std::vector<Switch> switches_;
    int knobW_ = 64;

    std::map<juce::String, float> currentParams_;
    bool isUpdatingFromPoll_ = false;
    /// Which plugin instance the pushes address (PLG-04); default All.
    std::unique_ptr<PluginTargetChooser> targetChooser_;
    /// WF-9: a chain another surface staged (Mixer Chain, onboarding), shown
    /// here - the plugin chain's one editor - to apply or discard.
    juce::Label stagedLabel_;
    juce::TextButton applyStagedButton_{TRANS("Apply to plugin")};
    juce::TextButton discardStagedButton_{TRANS("Discard")};
    void showStaged();
    ListenerHandle chainListener_;
    /// Re-applies every cached palette colour (the knobs and switches paint
    /// from the theme live). Called at construction and on each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
        canvas_.repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SignalFlowRackPanel)
};

} // namespace khensu
