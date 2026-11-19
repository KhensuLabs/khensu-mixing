// desktop-app/Source/ui/MixMasterEngine/SignalFlowRackPanel.cpp
#include "SignalFlowRackPanel.h"
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/ButtonRole.h"
#include <khensu/mixerchain/DspChainMapper.h>
#include <cmath>
#include <algorithm>

namespace khensu {

namespace {
} // namespace

// Helper Container Component to draw the block backgrounds and titles inside the Viewport
class RackContainer final : public juce::Component {
  public:
    RackContainer() = default;

    void paint(juce::Graphics& g) override
    {
        g.fillAll(KhensuTheme::bgBase);

        auto drawBlock = [&](const juce::String& title, int x, int y, int w, int h) {
            juce::Rectangle<int> r(x, y, w, h);
            g.setColour(KhensuTheme::bgRaised);
            g.fillRoundedRectangle(r.toFloat(), 6.0f);
            g.setColour(KhensuTheme::borderSubtle);
            g.drawRoundedRectangle(r.toFloat(), 6.0f, 1.0f);

            // Title block
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.85f));
            g.setFont(khensu::KhensuTheme::uiFont(11.0f, juce::Font::bold));
            g.drawText(title, x + 10, y + 8, w - 20, 16, juce::Justification::left);
        };

        drawBlock("HPF", 10, 10, 140, 200);
        drawBlock("LPF", 160, 10, 140, 200);
        drawBlock("EQ BAND 1", 310, 10, 130, 200);
        drawBlock("EQ BAND 2", 450, 10, 130, 200);
        drawBlock("EQ BAND 3", 590, 10, 130, 200);
        drawBlock("COMPRESSOR", 730, 10, 440, 200);
        drawBlock("LIMITER", 1180, 10, 140, 200);
        drawBlock("OUTPUT / GLOBAL", 1330, 10, 200, 200);

        // Draw vertical separator in Compressor block
        g.setColour(KhensuTheme::borderSubtle);
        g.fillRect(730 + 220, 15, 1, 190);
    }
};

SignalFlowRackPanel::SignalFlowRackPanel(IAppController& c) : PanelCard(c, "Visual Signal-Flow Rack")
{
    setExplainer(TRANS("DSP Signal-Flow Rack"),
                 TRANS("Directly controls the real-time built-in corrective C++ DSP engine inside the plugin. "
                 "Each stage can be bypassed or adjusted individually. "
                 "Any edits are posted in real-time to the VST3 plugin running in your DAW."));

    // Push target: all Copilot instances, or one (PLG-04).
    targetChooser_ = std::make_unique<PluginTargetChooser>(
        [this] { return controller_.lifecycle().knownPluginInstances(); }, [this] { return controller_.lifecycle().pluginTargetInstance(); },
        [this](const juce::String& id) { controller_.lifecycle().setPluginTargetInstance(id); });
    addAndMakeVisible(*targetChooser_);

    // Add horizontal viewport
    addAndMakeVisible(viewport_);
    viewport_.setViewedComponent(&container_, false);
    viewport_.setScrollBarsShown(false, true); // only horizontal

    // Recreate the layout inside our custom RackContainer
    auto* containerComp = new RackContainer();
    container_.addChildComponent(containerComp);
    containerComp->setVisible(true);

    using P = khensu::mixerchain::DspParamIds;

    // --- Add HPF ---
    addToggle(P::hpfOn, "Enable");
    addSlider(P::hpfFreq, "Freq", 20.0f, 1000.0f, 80.0f, true, " Hz");

    // --- Add LPF ---
    addToggle(P::lpfOn, "Enable");
    addSlider(P::lpfFreq, "Freq", 1000.0f, 20000.0f, 18000.0f, true, " Hz");

    // --- Add EQ Band 1 ---
    addToggle(P::eqOn, "Enable");
    addSlider(P::eqFreq, "Freq", 60.0f, 16000.0f, 1000.0f, true, " Hz");
    addSlider(P::eqGainDb, "Gain", -12.0f, 12.0f, 0.0f, false, " dB");
    addSlider(P::eqQ, "Q", 0.3f, 8.0f, 1.0f, false, "");

    // --- Add EQ Band 2 ---
    addToggle(P::eq2On, "Enable");
    addSlider(P::eq2Freq, "Freq", 60.0f, 16000.0f, 1000.0f, true, " Hz");
    addSlider(P::eq2GainDb, "Gain", -12.0f, 12.0f, 0.0f, false, " dB");
    addSlider(P::eq2Q, "Q", 0.3f, 8.0f, 1.0f, false, "");

    // --- Add EQ Band 3 ---
    addToggle(P::eq3On, "Enable");
    addSlider(P::eq3Freq, "Freq", 60.0f, 16000.0f, 1000.0f, true, " Hz");
    addSlider(P::eq3GainDb, "Gain", -12.0f, 12.0f, 0.0f, false, " dB");
    addSlider(P::eq3Q, "Q", 0.3f, 8.0f, 1.0f, false, "");

    // --- Add Compressor ---
    addToggle(P::compOn, "Enable");
    addToggle(P::compMultiband, "Multiband");
    addSlider(P::compThreshDb, "Thresh", -48.0f, 0.0f, -18.0f, false, " dB");
    addSlider(P::compLowThreshDb, "Low Thresh", -48.0f, 0.0f, -18.0f, false, " dB");
    addSlider(P::compMidThreshDb, "Mid Thresh", -48.0f, 0.0f, -18.0f, false, " dB");
    addSlider(P::compHighThreshDb, "High Thresh", -48.0f, 0.0f, -18.0f, false, " dB");
    addSlider(P::crossoverLowFreq, "XOver Low", 20.0f, 1000.0f, 200.0f, true, " Hz");
    addSlider(P::crossoverHighFreq, "XOver High", 1000.0f, 20000.0f, 2500.0f, true, " Hz");
    addSlider(P::compRatio, "Ratio", 1.0f, 20.0f, 4.0f, false, ":1");
    addSlider(P::compAttackMs, "Attack", 0.1f, 200.0f, 5.0f, true, " ms");
    addSlider(P::compReleaseMs, "Release", 5.0f, 1000.0f, 100.0f, true, " ms");

    // --- Add Limiter ---
    addToggle(P::limOn, "Enable");
    addSlider(P::limThreshDb, "Thresh", -12.0f, 0.0f, -1.0f, false, " dB");

    // --- Add Output/Global ---
    addToggle("bypass", "Bypass All");
    addSlider(P::chainMix, "Mix", 0.0f, 1.0f, 1.0f, false, "");
    addSlider(P::outGainDb, "Out Gain", -24.0f, 24.0f, 0.0f, false, " dB");

    // Initialize local map
    for (const auto& s : sliders_)
        currentParams_[s.paramId] = static_cast<float>(s.slider->getValue());
    for (const auto& t : toggles_)
        currentParams_[t.paramId] = t.button->getToggleState() ? 1.0f : 0.0f;

    // WF-9: a staged chain from another surface.
    stagedLabel_.setFont(KhensuTheme::labelFont(11.0f));
    stagedLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    addChildComponent(stagedLabel_);
    applyStagedButton_.setTooltip(TRANS("Send the staged chain to the plugin; the rack then shows it."));
    applyStagedButton_.onClick = [this] {
        controller_.lifecycle().applyStagedPluginChain();
        loadParamsFromServer();
    };
    button_role::markPrimary(applyStagedButton_);
    addChildComponent(applyStagedButton_);
    discardStagedButton_.onClick = [this] { controller_.lifecycle().discardStagedPluginChain(); };
    addChildComponent(discardStagedButton_);
    chainListener_ = listen(controller_.lifecycle(), this, &IServiceLifecycle::addPluginChainListener,
                            &IServiceLifecycle::removePluginChainListener, [this] { showStaged(); });
    showStaged();
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

SignalFlowRackPanel::~SignalFlowRackPanel() { stopTimer(); }

void SignalFlowRackPanel::paint(juce::Graphics& g) { PanelCard::paint(g); }

void SignalFlowRackPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    if (targetChooser_ != nullptr) {
        targetChooser_->setBounds(b.removeFromTop(22).removeFromLeft(juce::jmin(280, b.getWidth())));
        b.removeFromTop(4);
    }
    if (stagedLabel_.isVisible()) {
        auto row = b.removeFromTop(KhensuTheme::buttonHSmall);
        discardStagedButton_.setBounds(row.removeFromRight(80));
        row.removeFromRight(KhensuTheme::sp1);
        applyStagedButton_.setBounds(row.removeFromRight(130));
        row.removeFromRight(KhensuTheme::sp2);
        stagedLabel_.setBounds(row);
        b.removeFromTop(4);
    }
    viewport_.setBounds(b);

    const int containerW = 1540;
    const int containerH = b.getHeight() - 10;
    container_.setBounds(0, 0, containerW, containerH);

    if (auto* child = container_.getChildComponent(0)) child->setBounds(container_.getLocalBounds());

    // Position HPF
    if (auto* hpfOn = getToggle(mixerchain::DspParamIds::hpfOn)) hpfOn->setBounds(100, 16, 45, 16);
    if (auto* hpfFreq = getSlider(mixerchain::DspParamIds::hpfFreq)) hpfFreq->setBounds(15, 55, 130, 45);

    // Position LPF
    if (auto* lpfOn = getToggle(mixerchain::DspParamIds::lpfOn)) lpfOn->setBounds(250, 16, 45, 16);
    if (auto* lpfFreq = getSlider(mixerchain::DspParamIds::lpfFreq)) lpfFreq->setBounds(165, 55, 130, 45);

    // Position EQ 1
    if (auto* eqOn = getToggle(mixerchain::DspParamIds::eqOn)) eqOn->setBounds(390, 16, 45, 16);
    if (auto* eqFreq = getSlider(mixerchain::DspParamIds::eqFreq)) eqFreq->setBounds(315, 45, 120, 40);
    if (auto* eqGain = getSlider(mixerchain::DspParamIds::eqGainDb)) eqGain->setBounds(315, 95, 120, 40);
    if (auto* eqQ = getSlider(mixerchain::DspParamIds::eqQ)) eqQ->setBounds(315, 145, 120, 40);

    // Position EQ 2
    if (auto* eq2On = getToggle(mixerchain::DspParamIds::eq2On)) eq2On->setBounds(530, 16, 45, 16);
    if (auto* eq2Freq = getSlider(mixerchain::DspParamIds::eq2Freq)) eq2Freq->setBounds(455, 45, 120, 40);
    if (auto* eq2Gain = getSlider(mixerchain::DspParamIds::eq2GainDb)) eq2Gain->setBounds(455, 95, 120, 40);
    if (auto* eq2Q = getSlider(mixerchain::DspParamIds::eq2Q)) eq2Q->setBounds(455, 145, 120, 40);

    // Position EQ 3
    if (auto* eq3On = getToggle(mixerchain::DspParamIds::eq3On)) eq3On->setBounds(670, 16, 45, 16);
    if (auto* eq3Freq = getSlider(mixerchain::DspParamIds::eq3Freq)) eq3Freq->setBounds(595, 45, 120, 40);
    if (auto* eq3Gain = getSlider(mixerchain::DspParamIds::eq3GainDb)) eq3Gain->setBounds(595, 95, 120, 40);
    if (auto* eq3Q = getSlider(mixerchain::DspParamIds::eq3Q)) eq3Q->setBounds(595, 145, 120, 40);

    // Position Compressor
    if (auto* compOn = getToggle(mixerchain::DspParamIds::compOn)) compOn->setBounds(850, 16, 95, 16);
    if (auto* compMultiband = getToggle(mixerchain::DspParamIds::compMultiband))
        compMultiband->setBounds(1070, 16, 95, 16);

    // Column 1
    if (auto* compThresh = getSlider(mixerchain::DspParamIds::compThreshDb)) compThresh->setBounds(735, 45, 210, 32);
    if (auto* compRatio = getSlider(mixerchain::DspParamIds::compRatio)) compRatio->setBounds(735, 82, 210, 32);
    if (auto* compAttack = getSlider(mixerchain::DspParamIds::compAttackMs)) compAttack->setBounds(735, 119, 210, 32);
    if (auto* compRelease = getSlider(mixerchain::DspParamIds::compReleaseMs))
        compRelease->setBounds(735, 156, 210, 32);

    // Column 2
    if (auto* compLowThresh = getSlider(mixerchain::DspParamIds::compLowThreshDb))
        compLowThresh->setBounds(965, 34, 190, 30);
    if (auto* compMidThresh = getSlider(mixerchain::DspParamIds::compMidThreshDb))
        compMidThresh->setBounds(965, 66, 190, 30);
    if (auto* compHighThresh = getSlider(mixerchain::DspParamIds::compHighThreshDb))
        compHighThresh->setBounds(965, 98, 190, 30);
    if (auto* crossoverLow = getSlider(mixerchain::DspParamIds::crossoverLowFreq))
        crossoverLow->setBounds(965, 130, 190, 30);
    if (auto* crossoverHigh = getSlider(mixerchain::DspParamIds::crossoverHighFreq))
        crossoverHigh->setBounds(965, 162, 190, 30);

    // Position Limiter
    if (auto* limOn = getToggle(mixerchain::DspParamIds::limOn)) limOn->setBounds(1270, 16, 45, 16);
    if (auto* limThresh = getSlider(mixerchain::DspParamIds::limThreshDb)) limThresh->setBounds(1185, 55, 130, 45);

    // Position Output / Global
    if (auto* bypass = getToggle("bypass")) bypass->setBounds(1430, 16, 95, 16);
    if (auto* mix = getSlider(mixerchain::DspParamIds::chainMix)) mix->setBounds(1335, 55, 190, 45);
    if (auto* outGain = getSlider(mixerchain::DspParamIds::outGainDb)) outGain->setBounds(1335, 115, 190, 45);
}

void SignalFlowRackPanel::visibilityChanged()
{
    if (isVisible()) {
        loadParamsFromServer();
        startTimer(1000); // Poll once per second
    } else {
        stopTimer();
    }
}

void SignalFlowRackPanel::timerCallback()
{
    // Don't overwrite parameters while the user is physically interacting
    if (!isUserInteracting()) {
        loadParamsFromServer();
    }
}

void SignalFlowRackPanel::loadParamsFromServer()
{
    juce::Component::SafePointer<SignalFlowRackPanel> self(this);
    controller_.lifecycle().fetchPluginParams([self](const std::map<juce::String, float>& params) {
        if (self == nullptr) return;
        if (params.empty()) return;

        self->isUpdatingFromPoll_ = true;
        self->currentParams_ = params;

        for (const auto& s : self->sliders_) {
            auto it = params.find(s.paramId);
            if (it != params.end()) s.slider->setValue(it->second, juce::dontSendNotification);
        }

        for (const auto& t : self->toggles_) {
            auto it = params.find(t.paramId);
            if (it != params.end()) t.button->setToggleState(it->second > 0.5f, juce::dontSendNotification);
        }

        self->isUpdatingFromPoll_ = false;
    });
}

void SignalFlowRackPanel::pushParamsToServer() { controller_.lifecycle().pushPluginParams(currentParams_); }

void SignalFlowRackPanel::addSlider(const char* paramId, const juce::String& name, float min, float max, float defVal,
                                    bool isLog, const juce::String& suffix)
{
    DspSlider s;
    s.paramId = paramId;

    s.slider = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
    s.slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 16);

    if (isLog)
        s.slider->setRange(min, max);
    else
        s.slider->setRange(min, max, 0.1);

    if (isLog) s.slider->setSkewFactorFromMidPoint(std::sqrt(min * max));

    s.slider->setValue(defVal, juce::dontSendNotification);
    s.slider->setTextValueSuffix(suffix);

    s.slider->onValueChange = [this, id = juce::String(paramId)]() {
        if (!isUpdatingFromPoll_) {
            if (auto* sld = getSlider(id.toRawUTF8())) {
                currentParams_[id] = static_cast<float>(sld->getValue());
                pushParamsToServer();
            }
        }
    };

    container_.addAndMakeVisible(*s.slider);

    s.label = std::make_unique<juce::Label>();
    s.label->setText(name, juce::dontSendNotification);
    s.label->setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::plain));
    s.slider->addAndMakeVisible(*s.label);
    s.label->setBounds(0, -12, 100, 12); // Pin label above slider line

    sliders_.push_back(std::move(s));
    applyThemeColours(); // covers the control just added
}

void SignalFlowRackPanel::addToggle(const char* paramId, const juce::String& name)
{
    DspToggle t;
    t.paramId = paramId;

    t.button = std::make_unique<juce::ToggleButton>(name);

    t.button->onClick = [this, id = juce::String(paramId)]() {
        if (!isUpdatingFromPoll_) {
            if (auto* btn = getToggle(id.toRawUTF8())) {
                currentParams_[id] = btn->getToggleState() ? 1.0f : 0.0f;
                pushParamsToServer();
            }
        }
    };

    container_.addAndMakeVisible(*t.button);
    toggles_.push_back(std::move(t));
    applyThemeColours(); // covers the control just added
}

// Every cached palette colour on this panel, in one place (UI-01). The rack's
// sliders and toggles are BUILT AT RUNTIME from the DSP parameter list, so this
// walks the live vectors rather than naming fixed members - re-applying only
// named widgets would leave every generated control on the old palette.
void SignalFlowRackPanel::applyThemeColours()
{
    for (const auto& s : sliders_) {
        if (s.slider) {
            s.slider->setColour(juce::Slider::thumbColourId, KhensuTheme::accentTeal);
            s.slider->setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        }
        if (s.label) s.label->setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    }
    for (const auto& t : toggles_) {
        if (!t.button) continue;
        t.button->setColour(juce::ToggleButton::textColourId, KhensuTheme::textPrimary);
        t.button->setColour(juce::ToggleButton::tickColourId, KhensuTheme::accentTeal);
        t.button->setColour(juce::ToggleButton::tickDisabledColourId, KhensuTheme::textSecondary);
    }
}

juce::Slider* SignalFlowRackPanel::getSlider(const char* paramId) const
{
    for (const auto& s : sliders_)
        if (s.paramId == paramId) return s.slider.get();
    return nullptr;
}

juce::ToggleButton* SignalFlowRackPanel::getToggle(const char* paramId) const
{
    for (const auto& t : toggles_)
        if (t.paramId == paramId) return t.button.get();
    return nullptr;
}

bool SignalFlowRackPanel::isUserInteracting() const
{
    for (const auto& s : sliders_)
        if (s.slider->isMouseOverOrDragging()) return true;
    for (const auto& t : toggles_)
        if (t.button->isMouseOverOrDragging()) return true;
    return false;
}

} // namespace khensu
