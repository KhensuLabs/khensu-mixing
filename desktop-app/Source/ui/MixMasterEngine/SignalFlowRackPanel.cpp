// desktop-app/Source/ui/MixMasterEngine/SignalFlowRackPanel.cpp
#include "SignalFlowRackPanel.h"
#include "../../IAppController.h"
#include "../shared/ButtonRole.h"
#include "../shared/PanelRunState.h"
#include "../shared/TimelineCache.h"
#include <khensu/ui/ModuleViz.h>
#include <khensu/mixerchain/DspChainMapper.h>
#include <algorithm>
#include <cmath>

namespace khensu {


namespace {
constexpr int kModulePad = 10;
constexpr int kModuleGap = 16; // room for the signal-flow chevron
constexpr int kKnobGap = 6;
constexpr int kModuleHeaderH = 30;
/// VIZ-6a: each module shows what it does in a strip under its title.
constexpr int kVizH = 48;
constexpr int kRowGap = 10;
constexpr int kMinKnob = 50;
constexpr int kMaxKnob = 72;

int knobHeightFor(int knobW) { return juce::roundToInt(static_cast<float>(knobW) * 0.62f) + 34; }
/// The dial alone: too short for KhensuKnob to draw its own text (which uses
/// the fixed dark palette), so the rack paints value and label in theme colours.
int dialHeightFor(int knobW) { return juce::roundToInt(static_cast<float>(knobW) * 0.62f); }
constexpr int kValueRowH = 18;
constexpr int kLabelRowH = 14;

/// A module's power switch, or a small text switch (Multiband, Bypass).
/// Paints from the theme live, so a theme change needs nothing cached.
class RackSwitch final : public juce::ToggleButton {
  public:
    RackSwitch(const juce::String& text, bool power) : juce::ToggleButton(text), power_(power)
    {
        setClickingTogglesState(true);
    }

    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        juce::ignoreUnused(down);
        auto b = getLocalBounds().toFloat().reduced(1.0f);
        const bool on = getToggleState();
        if (power_) {
            const float d = juce::jmin(b.getWidth(), b.getHeight());
            auto c = b.withSizeKeepingCentre(d, d);
            g.setColour(on ? KhensuTheme::accentTeal : KhensuTheme::textTertiary.withAlpha(over ? 0.9f : 0.6f));
            juce::Path arc;
            const auto centre = c.getCentre();
            const float r = d * 0.36f;
            arc.addCentredArc(centre.x, centre.y, r, r, 0.0f, 0.7f, juce::MathConstants<float>::twoPi - 0.7f, true);
            g.strokePath(arc, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.drawLine(centre.x, c.getY() + d * 0.12f, centre.x, centre.y, 1.6f);
        } else {
            g.setColour(on ? KhensuTheme::accentTeal.withAlpha(0.18f) : KhensuTheme::bgVoid.withAlpha(0.35f));
            g.fillRoundedRectangle(b, b.getHeight() * 0.5f);
            g.setColour(on ? KhensuTheme::accentTeal : KhensuTheme::borderSubtle.brighter(over ? 0.3f : 0.0f));
            g.drawRoundedRectangle(b, b.getHeight() * 0.5f, 1.0f);
            g.setColour(on ? KhensuTheme::accentTeal : KhensuTheme::textSecondary);
            g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
            g.drawText(getButtonText(), b, juce::Justification::centred, false);
        }
        if (hasKeyboardFocus(false)) {
            g.setColour(KhensuTheme::focusRing);
            g.drawRoundedRectangle(b, power_ ? b.getHeight() * 0.5f : b.getHeight() * 0.5f, 1.0f);
        }
    }

  private:
    bool power_;
};

juce::String formatValue(double v, int unit)
{
    switch (unit) {
    case 0: // Hz
        if (v >= 10000.0) return juce::String(v / 1000.0, 1) + " kHz";
        if (v >= 1000.0) return juce::String(v / 1000.0, 2) + " kHz";
        return juce::String(juce::roundToInt(v)) + " Hz";
    case 1: // dB
        return juce::String(v, 1) + " dB";
    case 2: // ms
        return v < 10.0 ? juce::String(v, 1) + " ms" : juce::String(juce::roundToInt(v)) + " ms";
    case 3: // ratio
        return juce::String(v, 1) + ":1";
    case 4: // Q
        return juce::String(v, 2);
    default: // percent of a 0..1 value
        return juce::String(juce::roundToInt(v * 100.0)) + "%";
    }
}
} // namespace

SignalFlowRackPanel::SignalFlowRackPanel(IAppController& c) : PanelCard(c, TRANS("Signal chain"))
{
    setExplainer(TRANS("Signal chain"),
                 TRANS("The plugin's built-in corrective DSP chain, in signal order: high-pass, low-pass, three EQ "
                       "bands, compressor (optionally multiband), limiter and output. Switch a module on or off with "
                       "its power button and shape it with the knobs (drag; Shift-drag for fine; double-click resets). "
                       "Edits are sent in real time to the Copilot plugin running in your DAW."));

    // Push target: all Copilot instances, or one (PLG-04).
    targetChooser_ = std::make_unique<PluginTargetChooser>(
        [this] { return controller_.lifecycle().knownPluginInstances(); },
        [this] { return controller_.lifecycle().pluginTargetInstance(); },
        [this](const juce::String& id) { controller_.lifecycle().setPluginTargetInstance(id); });
    addAndMakeVisible(*targetChooser_);

    canvas_.painter = [this](juce::Graphics& g) { paintCanvas(g); };
    viewport_.setViewedComponent(&canvas_, false);
    viewport_.setScrollBarsShown(false, true); // sideways only, when the rack is narrower than its modules
    addAndMakeVisible(viewport_);

    using P = khensu::mixerchain::DspParamIds;

    const int hpf = addModule(TRANS("HPF"), P::hpfOn, 1, 0);
    addKnob(hpf, P::hpfFreq, TRANS("Freq"), 20.0f, 1000.0f, 80.0f, Unit::Hz);
    const int lpf = addModule(TRANS("LPF"), P::lpfOn, 1, 0);
    addKnob(lpf, P::lpfFreq, TRANS("Freq"), 1000.0f, 20000.0f, 18000.0f, Unit::Hz);

    const char* eqOn[] = {P::eqOn, P::eq2On, P::eq3On};
    const char* eqFreq[] = {P::eqFreq, P::eq2Freq, P::eq3Freq};
    const char* eqGain[] = {P::eqGainDb, P::eq2GainDb, P::eq3GainDb};
    const char* eqQ[] = {P::eqQ, P::eq2Q, P::eq3Q};
    for (int band = 0; band < 3; ++band) {
        const int m = addModule(TRANS("EQ band %n").replace("%n", juce::String(band + 1)), eqOn[band], 3, 0);
        addKnob(m, eqFreq[band], TRANS("Freq"), 60.0f, 16000.0f, 1000.0f, Unit::Hz);
        addKnob(m, eqGain[band], TRANS("Gain"), -12.0f, 12.0f, 0.0f, Unit::Db);
        addKnob(m, eqQ[band], TRANS("Q"), 0.3f, 8.0f, 1.0f, Unit::Q);
    }

    const int comp = addModule(TRANS("Compressor"), P::compOn, 9, 1);
    addSwitch(comp, P::compMultiband, TRANS("Multiband"), false);
    addKnob(comp, P::compThreshDb, TRANS("Thresh"), -48.0f, 0.0f, -18.0f, Unit::Db);
    addKnob(comp, P::compRatio, TRANS("Ratio"), 1.0f, 20.0f, 4.0f, Unit::Ratio);
    addKnob(comp, P::compAttackMs, TRANS("Attack"), 0.1f, 200.0f, 5.0f, Unit::Ms);
    addKnob(comp, P::compReleaseMs, TRANS("Release"), 5.0f, 1000.0f, 100.0f, Unit::Ms);
    addKnob(comp, P::compLowThreshDb, TRANS("Low"), -48.0f, 0.0f, -18.0f, Unit::Db, true);
    addKnob(comp, P::compMidThreshDb, TRANS("Mid"), -48.0f, 0.0f, -18.0f, Unit::Db, true);
    addKnob(comp, P::compHighThreshDb, TRANS("High"), -48.0f, 0.0f, -18.0f, Unit::Db, true);
    addKnob(comp, P::crossoverLowFreq, TRANS("X-over lo"), 20.0f, 1000.0f, 200.0f, Unit::Hz, true);
    addKnob(comp, P::crossoverHighFreq, TRANS("X-over hi"), 1000.0f, 20000.0f, 2500.0f, Unit::Hz, true);

    const int lim = addModule(TRANS("Limiter"), P::limOn, 1, 1);
    addKnob(lim, P::limThreshDb, TRANS("Ceiling"), -12.0f, 0.0f, -1.0f, Unit::Db);

    const int out = addModule(TRANS("Output"), nullptr, 2, 1);
    addSwitch(out, "bypass", TRANS("Bypass"), false);
    addKnob(out, P::chainMix, TRANS("Mix"), 0.0f, 1.0f, 1.0f, Unit::Percent);
    addKnob(out, P::outGainDb, TRANS("Gain"), -24.0f, 24.0f, 0.0f, Unit::Db);
    if (auto* g = knobFor(P::outGainDb)) static_cast<ui::KhensuKnob*>(g)->setBipolar(true);
    for (const char* id : {P::eqGainDb, P::eq2GainDb, P::eq3GainDb})
        if (auto* g = knobFor(id)) static_cast<ui::KhensuKnob*>(g)->setBipolar(true);

    for (const auto& k : knobs_)
        currentParams_[k.paramId] = static_cast<float>(k.knob->getValue());
    for (const auto& s : switches_)
        currentParams_[s.paramId] = s.button->getToggleState() ? 1.0f : 0.0f;
    updateModuleStates();

    // WF-9: a staged chain from another surface.
    stagedLabel_.setFont(KhensuTheme::bodyFont());
    addChildComponent(stagedLabel_);
    applyStagedButton_.setTooltip(TRANS("Send the staged chain to the plugin; the rack then shows it."));
    applyStagedButton_.onClick = [this] {
        controller_.lifecycle().applyStagedPluginChain();
        loadParamsFromServer();
    };
    button_role::markPrimary(applyStagedButton_);
    addChildComponent(applyStagedButton_);
    discardStagedButton_.setTooltip(TRANS("Drop the staged chain; the plugin keeps its current settings."));
    discardStagedButton_.onClick = [this] { controller_.lifecycle().discardStagedPluginChain(); };
    addChildComponent(discardStagedButton_);
    applyThemeColours();
    chainListener_ = listen(controller_.lifecycle(), this, &IServiceLifecycle::addPluginChainListener,
                            &IServiceLifecycle::removePluginChainListener, [this] { showStaged(); });
    showStaged();
}

SignalFlowRackPanel::~SignalFlowRackPanel() { stopTimer(); }

void SignalFlowRackPanel::applyThemeColours()
{
    stagedLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
}

int SignalFlowRackPanel::addModule(const juce::String& title, const char* onParam, int slots, int row)
{
    Module m;
    m.title = title;
    m.onParam = onParam != nullptr ? juce::String(onParam) : juce::String();
    m.spec = {slots, row};
    modules_.push_back(m);
    const int index = static_cast<int>(modules_.size()) - 1;
    if (onParam != nullptr) addSwitch(index, onParam, title, true);
    return index;
}

void SignalFlowRackPanel::addKnob(int module, const char* paramId, const juce::String& label, float min, float max,
                                  float defVal, Unit unit, bool bandOnly)
{
    Knob k;
    k.paramId = paramId;
    k.module = module;
    k.bandOnly = bandOnly;
    k.knob = std::make_unique<ui::KhensuKnob>(label);
    auto& knob = *k.knob;
    const bool log = unit == Unit::Hz || unit == Unit::Ms;
    knob.setRange(min, max, log ? 0.0 : (unit == Unit::Q ? 0.01 : 0.1));
    if (log) knob.setSkewFactorFromMidPoint(std::sqrt(static_cast<double>(min) * static_cast<double>(max)));
    knob.setDefaultValue(defVal);
    knob.setValue(defVal, juce::dontSendNotification);
    const int unitIndex = static_cast<int>(unit);
    knob.textFromValueFunction = [unitIndex](double v) { return formatValue(v, unitIndex); };
    knob.valueFromTextFunction = [](const juce::String& text) { return text.getDoubleValue(); };
    const auto& moduleTitle = modules_[static_cast<size_t>(module)].title;
    knob.setTitle(moduleTitle + " " + label);
    knob.setTooltip(moduleTitle + ": " + label + " - " + TRANS("drag, Shift-drag for fine, double-click to reset"));
    knob.onValueChange = [this, id = juce::String(paramId)] {
        canvas_.repaint(); // the painted value
        if (isUpdatingFromPoll_) return;
        if (auto* s = knobFor(id)) {
            currentParams_[id] = static_cast<float>(s->getValue());
            pushParamsToServer();
        }
    };
    canvas_.addAndMakeVisible(knob);
    knobs_.push_back(std::move(k));
}

void SignalFlowRackPanel::addSwitch(int module, const char* paramId, const juce::String& label, bool power)
{
    Switch s;
    s.paramId = paramId;
    s.module = module;
    s.power = power;
    s.button = std::make_unique<RackSwitch>(power ? juce::String() : label, power);
    s.button->setTitle(power ? label + " " + TRANS("on") : label);
    s.button->setTooltip(power ? TRANS("Switch %s on or off").replace("%s", label)
                               : (juce::String(paramId) == "bypass"
                                      ? TRANS("Bypass the whole chain (the plugin passes audio through untouched)")
                                      : TRANS("Split the compressor into three bands")));
    s.button->onClick = [this, id = juce::String(paramId)] {
        if (isUpdatingFromPoll_) return;
        if (auto* b = switchFor(id)) {
            currentParams_[id] = b->getToggleState() ? 1.0f : 0.0f;
            updateModuleStates();
            pushParamsToServer();
        }
    };
    canvas_.addAndMakeVisible(*s.button);
    switches_.push_back(std::move(s));
}

juce::Slider* SignalFlowRackPanel::knobFor(const juce::String& paramId) const
{
    for (const auto& k : knobs_)
        if (k.paramId == paramId) return k.knob.get();
    return nullptr;
}

juce::Button* SignalFlowRackPanel::switchFor(const juce::String& paramId) const
{
    for (const auto& s : switches_)
        if (s.paramId == paramId) return s.button.get();
    return nullptr;
}

bool SignalFlowRackPanel::moduleOn(int module) const
{
    const auto& m = modules_[static_cast<size_t>(module)];
    if (m.onParam.isEmpty()) {
        auto* bypass = switchFor("bypass");
        return bypass == nullptr || !bypass->getToggleState();
    }
    auto* s = switchFor(m.onParam);
    return s == nullptr || s->getToggleState();
}

void SignalFlowRackPanel::updateModuleStates()
{
    auto* multiband = switchFor(mixerchain::DspParamIds::compMultiband);
    const bool bands = multiband != nullptr && multiband->getToggleState();
    // An off module stays editable (dialled in before switching it on), just quieter.
    for (const auto& k : knobs_) {
        const bool live = moduleOn(k.module) && (!k.bandOnly || bands);
        k.knob->setAlpha(live ? 1.0f : 0.42f);
    }
    canvas_.repaint();
}

void SignalFlowRackPanel::showStaged()
{
    const auto staged = controller_.lifecycle().stagedPluginChain();
    const bool show = staged.has_value();
    if (show) {
        int on = 0;
        for (const auto& [id, v] : staged->params)
            if (id.endsWith("_on") && v > 0.5f) ++on;
        stagedLabel_.setText(TRANS("Staged from %s: %n stage(s) on. Apply it to the plugin, or discard it.")
                                 .replace("%s", staged->source)
                                 .replace("%n", juce::String(on)),
                             juce::dontSendNotification);
    }
    stagedLabel_.setVisible(show);
    applyStagedButton_.setVisible(show);
    discardStagedButton_.setVisible(show);
    resized();
}

int SignalFlowRackPanel::preferredHeight(int width) const
{
    std::vector<rack_layout::ModuleSpec> specs;
    for (const auto& m : modules_)
        specs.push_back(m.spec);
    const int inner = width - 2 * KhensuTheme::sp4;
    const int knobW = rack_layout::knobWidthFor(specs, inner, kModulePad, kModuleGap, kKnobGap, kMinKnob, kMaxKnob);
    const int rowH = kModuleHeaderH + kVizH + knobHeightFor(knobW) + kModulePad;
    int h = PanelCard::collapsedHeight() + 2 * rowH + kRowGap + 2 * KhensuTheme::sp4;
    if (stagedLabel_.isVisible()) h += KhensuTheme::buttonHSmall + 6;
    if (knobW == kMinKnob) h += 10; // the sideways scrollbar
    return h;
}

void SignalFlowRackPanel::paint(juce::Graphics& g) { PanelCard::paint(g); }

void SignalFlowRackPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    if (targetChooser_ != nullptr) {
        // In the card's header, left of its explainer button.
        auto header = getLocalBounds().removeFromTop(PanelCard::collapsedHeight()).reduced(0, 4);
        header.removeFromRight(KhensuTheme::sp4 + 30);
        targetChooser_->setBounds(header.removeFromRight(juce::jmin(220, header.getWidth() / 2)));
    }
    if (stagedLabel_.isVisible()) {
        auto row = b.removeFromTop(KhensuTheme::buttonHSmall);
        discardStagedButton_.setBounds(row.removeFromRight(84));
        row.removeFromRight(KhensuTheme::sp1);
        applyStagedButton_.setBounds(row.removeFromRight(130));
        row.removeFromRight(KhensuTheme::sp2);
        stagedLabel_.setBounds(row);
        b.removeFromTop(6);
    }
    viewport_.setBounds(b);
    layoutCanvas(b.getWidth());
}

void SignalFlowRackPanel::layoutCanvas(int width)
{
    std::vector<rack_layout::ModuleSpec> specs;
    for (const auto& m : modules_)
        specs.push_back(m.spec);
    knobW_ = rack_layout::knobWidthFor(specs, width, kModulePad, kModuleGap, kKnobGap, kMinKnob, kMaxKnob);
    const int rowH = kModuleHeaderH + kVizH + knobHeightFor(knobW_) + kModulePad;
    const int contentW = juce::jmax(rack_layout::rowWidth(specs, 0, knobW_, kModulePad, kModuleGap, kKnobGap),
                                    rack_layout::rowWidth(specs, 1, knobW_, kModulePad, kModuleGap, kKnobGap));
    const int canvasW = juce::jmax(width, contentW);
    canvas_.setSize(canvasW, 2 * rowH + kRowGap);

    // Each row is spread to the full width: spare room goes to the modules'
    // padding so the rows line up at both edges.
    for (int row = 0; row < 2; ++row) {
        const int used = rack_layout::rowWidth(specs, row, knobW_, kModulePad, kModuleGap, kKnobGap);
        int count = 0;
        for (const auto& m : modules_)
            if (m.spec.row == row) ++count;
        const int spare = count > 0 ? (canvasW - used) / count : 0;
        int x = 0;
        const int y = row * (rowH + kRowGap);
        for (size_t i = 0; i < modules_.size(); ++i) {
            auto& m = modules_[i];
            if (m.spec.row != row) continue;
            const int w = m.spec.slots * knobW_ + (m.spec.slots - 1) * kKnobGap + 2 * kModulePad + spare;
            m.bounds = {x, y, w, rowH};
            x += w + kModuleGap;

            // Header: the power switch, the title, a text switch at the right.
            auto header = m.bounds.withHeight(kModuleHeaderH).reduced(kModulePad - 2, 5);
            for (auto& s : switches_) {
                if (s.module != static_cast<int>(i)) continue;
                if (s.power)
                    s.button->setBounds(header.removeFromLeft(20));
                else
                    s.button->setBounds(header.removeFromRight(juce::jmin(82, header.getWidth() / 2)));
            }

            // Knobs, centred in the module's body.
            auto body = m.bounds.withTrimmedTop(kModuleHeaderH + kVizH).withTrimmedBottom(kModulePad);
            int k = 0;
            const int knobsW = m.spec.slots * knobW_ + (m.spec.slots - 1) * kKnobGap;
            const int kx0 = body.getX() + (body.getWidth() - knobsW) / 2;
            for (auto& kn : knobs_) {
                if (kn.module != static_cast<int>(i)) continue;
                kn.knob->setBounds(kx0 + k * (knobW_ + kKnobGap), body.getY(), knobW_, dialHeightFor(knobW_));
                ++k;
            }
        }
    }
    canvas_.repaint();
}

void SignalFlowRackPanel::paintCanvas(juce::Graphics& g)
{
    auto* multiband = switchFor(mixerchain::DspParamIds::compMultiband);
    const bool bands = multiband != nullptr && multiband->getToggleState();
    for (size_t i = 0; i < modules_.size(); ++i) {
        const auto& m = modules_[i];
        const bool on = moduleOn(static_cast<int>(i));
        auto r = m.bounds.toFloat().reduced(0.5f);
        g.setColour(KhensuTheme::bgRaised.withAlpha(on ? 0.75f : 0.4f));
        g.fillRoundedRectangle(r, 6.0f);
        g.setColour(on ? KhensuTheme::borderActive.withAlpha(0.6f) : KhensuTheme::borderSubtle);
        g.drawRoundedRectangle(r, 6.0f, 1.0f);

        // Title, right of the power switch.
        auto header = m.bounds.withHeight(kModuleHeaderH).reduced(kModulePad - 2, 5);
        if (m.onParam.isNotEmpty()) header.removeFromLeft(26);
        g.setColour(on ? KhensuTheme::textPrimary : KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::bodyFont(KhensuTheme::kFontBody).boldened());
        g.drawText(m.title, header, juce::Justification::centredLeft, true);

        paintModuleViz(g, static_cast<int>(i), m.bounds.withTrimmedTop(kModuleHeaderH).withHeight(kVizH)
                                                    .reduced(kModulePad - 2, 3).toFloat(), on);

        // Each knob's value (largest, tabular) and name under its dial.
        for (const auto& k : knobs_) {
            if (k.module != static_cast<int>(i)) continue;
            const auto kb = k.knob->getBounds();
            const float alpha = k.knob->getAlpha();
            auto text = juce::Rectangle<int>(kb.getX() - 4, kb.getBottom() + 2, kb.getWidth() + 8, kValueRowH);
            g.setColour(KhensuTheme::textPrimary.withMultipliedAlpha(alpha));
            g.setFont(KhensuTheme::numericFont(KhensuTheme::kFontBody));
            g.drawFittedText(k.knob->getTextFromValue(k.knob->getValue()), text, juce::Justification::centred, 1, 0.8f);
            g.setColour(KhensuTheme::textTertiary.withMultipliedAlpha(alpha));
            g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
            g.drawFittedText(static_cast<const ui::KhensuKnob*>(k.knob.get())->getLabel(),
                             text.translated(0, kValueRowH).withHeight(kLabelRowH), juce::Justification::centred, 1, 0.8f);
        }

        // The compressor's band controls sit apart, behind a hairline.
        bool sawBand = false;
        for (const auto& k : knobs_) {
            if (k.module != static_cast<int>(i) || !k.bandOnly || sawBand) continue;
            sawBand = true;
            const float x = static_cast<float>(k.knob->getX()) - kKnobGap * 0.5f;
            g.setColour(KhensuTheme::borderSubtle);
            g.fillRect(x, static_cast<float>(m.bounds.getY() + kModuleHeaderH + kVizH + 4), 1.0f,
                       static_cast<float>(k.knob->getHeight() + kValueRowH + kLabelRowH - 8));
            g.setColour(bands ? KhensuTheme::textSecondary : KhensuTheme::textTertiary);
            g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontCaption));
            g.drawText(TRANS("Bands"), juce::Rectangle<float>(x + 4.0f, static_cast<float>(m.bounds.getY() + 7),
                                                              60.0f, 16.0f),
                       juce::Justification::centredLeft, false);
        }

        // Signal flow: a chevron into the next module of the same row.
        if (i + 1 < modules_.size() && modules_[i + 1].spec.row == m.spec.row) {
            const float cx = static_cast<float>(m.bounds.getRight()) + kModuleGap * 0.5f;
            const float cy = static_cast<float>(m.bounds.getY() + kModuleHeaderH + (m.bounds.getHeight() - kModuleHeaderH) / 2);
            juce::Path chevron;
            chevron.startNewSubPath(cx - 3.0f, cy - 5.0f);
            chevron.lineTo(cx + 2.0f, cy);
            chevron.lineTo(cx - 3.0f, cy + 5.0f);
            g.setColour(KhensuTheme::textTertiary);
            g.strokePath(chevron, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }
}

float SignalFlowRackPanel::paramValue(const char* id, float fallback) const
{
    if (auto* k = knobFor(id)) return static_cast<float>(k->getValue());
    const auto it = currentParams_.find(id);
    return it != currentParams_.end() ? it->second : fallback;
}

void SignalFlowRackPanel::paintModuleViz(juce::Graphics& g, int module, juce::Rectangle<float> r, bool on)
{
    using P = khensu::mixerchain::DspParamIds;
    auto style = ui::module_viz::Style::fromPalette();
    style.background = KhensuTheme::bgVoid.withAlpha(0.45f);
    style.grid = KhensuTheme::vizGrid;
    style.curve = KhensuTheme::textPrimary;
    style.accent = on ? KhensuTheme::accentTeal : KhensuTheme::textTertiary;
    style.text = KhensuTheme::textTertiary;
    style.warn = KhensuTheme::accentAmber;
    const double sr = controller_.hasAnalysis() && controller_.currentAnalysis().sample_rate > 0.0
                          ? controller_.currentAnalysis().sample_rate
                          : 48000.0;
    const auto& onParam = modules_[static_cast<size_t>(module)].onParam;
    auto caption = [&](const juce::String& text) {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontCaption));
        g.drawText(text, r.reduced(4.0f, 1.0f), juce::Justification::bottomRight, false);
    };
    if (onParam == P::hpfOn || onParam == P::lpfOn) {
        EqBand b;
        b.type = onParam == P::hpfOn ? EqBandType::HighPass : EqBandType::LowPass;
        b.hz = paramValue(onParam == P::hpfOn ? P::hpfFreq : P::lpfFreq, onParam == P::hpfOn ? 80.0f : 18000.0f);
        ui::module_viz::paintEqResponse(g, r, {b}, sr, style);
        canvas_.hover.add(r, (onParam == P::hpfOn ? TRANS("High-pass") : TRANS("Low-pass")) + " " + TRANS("at") + " "
                                 + juce::String(juce::roundToInt(b.hz)) + " Hz, 12 dB/oct");
        return;
    }
    const char* eqOn[] = {P::eqOn, P::eq2On, P::eq3On};
    const char* eqFreq[] = {P::eqFreq, P::eq2Freq, P::eq3Freq};
    const char* eqGain[] = {P::eqGainDb, P::eq2GainDb, P::eq3GainDb};
    const char* eqQ[] = {P::eqQ, P::eq2Q, P::eq3Q};
    for (int band = 0; band < 3; ++band)
        if (onParam == eqOn[band]) {
            EqBand b;
            b.hz = paramValue(eqFreq[band], 1000.0f);
            b.gain_db = paramValue(eqGain[band], 0.0f);
            b.q = paramValue(eqQ[band], 1.0f);
            ui::module_viz::paintEqResponse(g, r, {b}, sr, style);
            canvas_.hover.add(r, TRANS("EQ band") + " " + juce::String(band + 1) + ": " + juce::String(juce::roundToInt(b.hz))
                                     + " Hz, " + (b.gain_db >= 0.0f ? "+" : "") + juce::String(b.gain_db, 1) + " dB, Q "
                                     + juce::String(b.q, 2));
            return;
        }
    if (onParam == P::compOn) {
        // The transfer curve; the dot is this track's typical level (its
        // integrated loudness), the meter the reduction that level gets.
        ui::module_viz::CompressorState c;
        c.thresholdDb = paramValue(P::compThreshDb, -18.0f);
        c.ratio = paramValue(P::compRatio, 4.0f);
        if (controller_.hasAnalysis() && controller_.currentAnalysis().has_fast_analysis()) {
            const float in = controller_.currentAnalysis().loudness.lufs_integrated;
            c.inputDb = in;
            c.reductionDb = std::max(0.0f, in - ui::module_viz::compressorOutputDb(in, c.thresholdDb, c.ratio, c.kneeDb));
        }
        auto plot = r.removeFromLeft(std::min(r.getWidth(), r.getHeight() * 1.6f));
        ui::module_viz::paintCompressor(g, plot, c, style);
        if (c.reductionDb)
            caption(TRANS("~%v dB reduction at this track's level").replace("%v", juce::String(*c.reductionDb, 1)));
        canvas_.hover.add(r.getUnion(plot), TRANS("Threshold") + " " + juce::String(c.thresholdDb, 1) + " dB, "
                                                + TRANS("ratio") + " " + juce::String(c.ratio, 1) + ":1"
                                                + (c.inputDb ? ", " + TRANS("this track at") + " " + juce::String(*c.inputDb, 1)
                                                                   + " LUFS: ~" + juce::String(c.reductionDb.value_or(0.0f), 1)
                                                                   + " dB " + TRANS("of reduction")
                                                             : juce::String()));
        return;
    }
    if (onParam == P::limOn) {
        // Gain-reduction history: how far this track's peaks rise over the
        // ceiling, across the whole track (VIZ-6a).
        const float ceiling = paramValue(P::limThreshDb, -1.0f);
        std::vector<float> history;
        if (const auto tl = timeline_cache::find(controller_.currentAuditionPath()); tl != nullptr) {
            const size_t n = tl->peakDbfs.size(), cols = static_cast<size_t>(std::max(8, juce::roundToInt(r.getWidth())));
            history.assign(cols, 0.0f);
            for (size_t i = 0; i < n; ++i) {
                const size_t c = std::min(cols - 1, i * cols / std::max<size_t>(1, n));
                history[c] = std::max(history[c], std::max(0.0f, tl->peakDbfs[i] - ceiling));
            }
        } else if (!timelineRequested_ && controller_.currentAuditionPath().isNotEmpty()) {
            timelineRequested_ = true;
            const auto path = controller_.currentAuditionPath();
            fetchQuietly(*this, controller_, [path] { return timeline_cache::measure(path); },
                         [](SignalFlowRackPanel& self, timeline_cache::Ptr) { self.canvas_.repaint(); },
                         [](SignalFlowRackPanel& self) { self.timelineRequested_ = false; }, true);
        }
        ui::module_viz::paintReductionHistory(g, r, history, style);
        {
            int over = 0;
            for (const float v : history)
                if (v > 0.0f) ++over;
            canvas_.hover.add(r, TRANS("Ceiling") + " " + juce::String(ceiling, 1) + " dB: "
                                     + (history.empty() ? TRANS("measuring the track...")
                                                        : juce::String(juce::roundToInt(100.0f * (float)over / (float)history.size()))
                                                              + " % " + TRANS("of the track has peaks over it")));
        }
        return;
    }
    // Output: the chain's mix and gain as two bars.
    const float mix = paramValue(P::chainMix, 1.0f), gain = paramValue(P::outGainDb, 0.0f);
    auto top = r.removeFromTop(r.getHeight() * 0.5f).reduced(0.0f, 4.0f);
    auto bottom = r.reduced(0.0f, 4.0f);
    g.setColour(style.background);
    g.fillRoundedRectangle(top, 2.0f);
    g.fillRoundedRectangle(bottom, 2.0f);
    g.setColour(style.accent);
    g.fillRoundedRectangle(top.withWidth(top.getWidth() * juce::jlimit(0.0f, 1.0f, mix)), 2.0f);
    const float mid = bottom.getCentreX(), x = mid + bottom.getWidth() * 0.5f * juce::jlimit(-1.0f, 1.0f, gain / 24.0f);
    g.fillRect(juce::Rectangle<float>(std::min(mid, x), bottom.getY(), std::max(2.0f, std::abs(x - mid)), bottom.getHeight()));
    canvas_.hover.add(top.getUnion(bottom), TRANS("Mix") + " " + juce::String(juce::roundToInt(mix * 100.0f)) + " %, "
                                                + TRANS("output gain") + " " + (gain >= 0.0f ? "+" : "") + juce::String(gain, 1)
                                                + " dB");
}

void SignalFlowRackPanel::visibilityChanged()
{
    if (isVisible()) {
        loadParamsFromServer();
        startTimer(1000); // poll once per second
    } else {
        stopTimer();
    }
}

void SignalFlowRackPanel::timerCallback()
{
    // Don't overwrite parameters while the user is physically interacting.
    if (!isUserInteracting()) loadParamsFromServer();
}

void SignalFlowRackPanel::applyParams(const std::map<juce::String, float>& params)
{
    isUpdatingFromPoll_ = true;
    for (const auto& [id, v] : params)
        currentParams_[id] = v;
    for (const auto& k : knobs_)
        if (auto it = params.find(k.paramId); it != params.end())
            k.knob->setValue(it->second, juce::dontSendNotification);
    for (const auto& s : switches_)
        if (auto it = params.find(s.paramId); it != params.end())
            s.button->setToggleState(it->second > 0.5f, juce::dontSendNotification);
    isUpdatingFromPoll_ = false;
    updateModuleStates();
}

void SignalFlowRackPanel::loadParamsFromServer()
{
    juce::Component::SafePointer<SignalFlowRackPanel> self(this);
    controller_.lifecycle().fetchPluginParams([self](const std::map<juce::String, float>& params) {
        if (self == nullptr || params.empty()) return;
        self->applyParams(params);
    });
}

void SignalFlowRackPanel::pushParamsToServer() { controller_.lifecycle().pushPluginParams(currentParams_); }

bool SignalFlowRackPanel::isUserInteracting() const
{
    for (const auto& k : knobs_)
        if (k.knob->isMouseOverOrDragging()) return true;
    for (const auto& s : switches_)
        if (s.button->isMouseOverOrDragging()) return true;
    return false;
}

} // namespace khensu
