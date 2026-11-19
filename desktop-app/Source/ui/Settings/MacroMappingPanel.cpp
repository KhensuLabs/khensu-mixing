// desktop-app/Source/ui/Settings/MacroMappingPanel.cpp
#include "MacroMappingPanel.h"
#include "../../IAppController.h"
#include "../../midi/MacroControlRouter.h"

namespace khensu {

namespace {
using Engine = MacroMappingEngine;

class MacroMappingWindow final : public juce::DocumentWindow {
  public:
    explicit MacroMappingWindow(IAppController& c)
        : juce::DocumentWindow("MIDI / OSC Macro Mapping", KhensuTheme::bgBase, juce::DocumentWindow::closeButton)
    {
        setContentOwned(new MacroMappingPanel(c), false);
        setResizable(true, false);
        setUsingNativeTitleBar(true);
        centreWithSize(820, 420);
    }
    void closeButtonPressed() override { setVisible(false); }
};
} // namespace

std::unique_ptr<juce::DocumentWindow> MacroMappingPanel::createWindow(IAppController& c)
{
    return std::make_unique<MacroMappingWindow>(c);
}

void MacroMappingPanel::applyThemeColours()
{
    header_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    oscNote_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    status_.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
    for (auto& r : rows_) {
        r.name.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
        r.source.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
        r.learn.setColour(juce::TextButton::buttonOnColourId, KhensuTheme::accentAmber.withAlpha(0.5f));
    }
}

MacroMappingPanel::MacroMappingPanel(IAppController& c) : controller_(c), router_(c.shell().macroRouter())
{
    applyThemeColours();
    setTitle(TRANS("MIDI and OSC macro mapping"));
    setFocusContainerType(juce::Component::FocusContainerType::focusContainer);

    header_.setText(TRANS("Map hardware controls or OSC to the vocal macros"), juce::dontSendNotification);
    header_.setFont(KhensuTheme::uiFont(KhensuTheme::kFontH2, juce::Font::bold));
    addAndMakeVisible(header_);

    const auto& targets = router_.engine().targets();
    for (int i = 0; i < kRows && i < targets.size(); ++i) {
        auto& r = rows_[static_cast<size_t>(i)];
        const auto target = targets[i];
        r.name.setText(target, juce::dontSendNotification);
        r.name.setFont(KhensuTheme::bodyFont(12.0f));
        r.source.setFont(KhensuTheme::labelFont(11.0f));
        r.learn.setClickingTogglesState(true);
        r.learn.setTitle(TRANS("Learn ") + target);
        r.learn.setTooltip(TRANS("Arm, then move a MIDI control or send an OSC message to bind it to ") + target + ".");
        r.learn.onClick = [this, i, target] {
            auto& row = rows_[static_cast<size_t>(i)];
            if (row.learn.getToggleState()) {
                for (int j = 0; j < kRows; ++j)
                    if (j != i) rows_[static_cast<size_t>(j)].learn.setToggleState(false, juce::dontSendNotification);
                router_.engine().armLearn(target);
                learnArmed_ = true;
                status_.setText(TRANS("Learning ") + target + ": move a control or send an OSC message...",
                                juce::dontSendNotification);
            } else {
                router_.engine().cancelLearn();
                learnArmed_ = false;
                status_.setText({}, juce::dontSendNotification);
            }
        };
        for (auto* s : {&r.minSlider, &r.maxSlider}) {
            s->setRange(0.0, 100.0, 1.0);
            s->setTextBoxStyle(juce::Slider::TextBoxLeft, false, 34, 18);
            s->onValueChange = [this, i] { commitRow(i); };
        }
        r.minSlider.setTitle(target + " range minimum");
        r.minSlider.setTooltip(TRANS("Macro value when the control is at its minimum."));
        r.maxSlider.setTitle(target + " range maximum");
        r.maxSlider.setTooltip(TRANS("Macro value when the control is at its maximum."));
        r.curve.addItemList({"Linear", "Exponential", "Logarithmic", "S-curve"}, 1);
        r.curve.setTitle(target + " response curve");
        r.curve.setTooltip(TRANS("Exponential = finer control at the bottom; Logarithmic = finer at the top."));
        r.curve.onChange = [this, i] { commitRow(i); };
        r.invert.setTitle(TRANS("Invert ") + target);
        r.invert.setTooltip(TRANS("Reverse the control's direction."));
        r.invert.onClick = [this, i] { commitRow(i); };
        r.clear.setTitle(TRANS("Clear ") + target + " mapping");
        r.clear.setTooltip(TRANS("Remove this macro's mapping."));
        r.clear.onClick = [this, i] {
            const int idx = mappingIndexFor(i);
            if (idx >= 0) router_.engine().removeMapping(static_cast<size_t>(idx));
            router_.save();
            refresh();
        };
        for (juce::Component* comp : std::initializer_list<juce::Component*>{
                 &r.name, &r.source, &r.learn, &r.minSlider, &r.maxSlider, &r.curve, &r.invert, &r.clear})
            addAndMakeVisible(comp);
    }

    oscToggle_.setTooltip(TRANS("Listen for OSC on the port below. Off by default: enabling opens a UDP port."));
    oscToggle_.onClick = [this] {
        if (oscToggle_.getToggleState()) {
            const int port = oscPort_.getText().getIntValue();
            if (!router_.enableOsc(port, lanToggle_.getToggleState())) {
                oscToggle_.setToggleState(false, juce::dontSendNotification);
                status_.setText(TRANS("Could not open OSC port ") + juce::String(port) + " (use 1024-65535, not in use).",
                                juce::dontSendNotification);
                return;
            }
        } else {
            router_.disableOsc();
        }
        router_.save();
        refresh();
    };
    addAndMakeVisible(oscToggle_);
    lanToggle_.setTooltip(TRANS("OSC has no authentication. Leave off to accept messages from this computer only."));
    lanToggle_.onClick = [this] {
        if (router_.oscEnabled()) {
            const int port = router_.oscPort();
            if (!router_.enableOsc(port, lanToggle_.getToggleState()))
                status_.setText(TRANS("Could not reopen OSC port ") + juce::String(port) + ".", juce::dontSendNotification);
            router_.save();
            refresh();
        }
    };
    addAndMakeVisible(lanToggle_);
    oscPort_.setInputRestrictions(5, "0123456789");
    oscPort_.setTitle(TRANS("OSC port"));
    oscPort_.setText(juce::String(MacroControlRouter::kDefaultOscPort), juce::dontSendNotification);
    addAndMakeVisible(oscPort_);
    oscNote_.setFont(KhensuTheme::labelFont(10.0f));
    oscNote_.setText(TRANS("Any address via Learn; arg float 0-1 or int 0-127."), juce::dontSendNotification);
    addAndMakeVisible(oscNote_);
    status_.setFont(KhensuTheme::labelFont(11.0f));
    addAndMakeVisible(status_);

    router_.onChange = [safe = juce::Component::SafePointer<MacroMappingPanel>(this)] {
        if (safe == nullptr) return;
        if (!safe->router_.engine().learnTarget()) {
            for (auto& r : safe->rows_)
                r.learn.setToggleState(false, juce::dontSendNotification);
            if (safe->learnArmed_) {
                safe->learnArmed_ = false;
                safe->router_.save();
                safe->status_.setText(TRANS("Mapped."), juce::dontSendNotification);
            }
        }
        safe->refresh();
    };
    refresh();
}

MacroMappingPanel::~MacroMappingPanel()
{
    router_.engine().cancelLearn();
    router_.onChange = nullptr;
}

int MacroMappingPanel::mappingIndexFor(int row) const
{
    const auto target = rows_[static_cast<size_t>(row)].name.getText();
    const auto& maps = router_.engine().mappings();
    for (size_t i = 0; i < maps.size(); ++i)
        if (maps[i].target == target) return static_cast<int>(i);
    return -1;
}

void MacroMappingPanel::commitRow(int row)
{
    if (refreshing_) return;
    const int idx = mappingIndexFor(row);
    if (idx < 0) return; // nothing bound yet - Learn first
    auto m = router_.engine().mappings()[static_cast<size_t>(idx)];
    auto& r = rows_[static_cast<size_t>(row)];
    m.minValue = r.minSlider.getValue();
    m.maxValue = r.maxSlider.getValue();
    m.curve = static_cast<Engine::Curve>(juce::jmax(0, r.curve.getSelectedItemIndex()));
    m.invert = r.invert.getToggleState();
    router_.engine().updateMapping(static_cast<size_t>(idx), m);
    router_.save();
}

void MacroMappingPanel::refresh()
{
    const juce::ScopedValueSetter<bool> guard(refreshing_, true);
    for (int i = 0; i < kRows; ++i) {
        auto& r = rows_[static_cast<size_t>(i)];
        const int idx = mappingIndexFor(i);
        const bool bound = idx >= 0;
        if (bound) {
            const auto& m = router_.engine().mappings()[static_cast<size_t>(idx)];
            r.source.setText(m.source.describe(), juce::dontSendNotification);
            r.minSlider.setValue(m.minValue, juce::dontSendNotification);
            r.maxSlider.setValue(m.maxValue, juce::dontSendNotification);
            r.curve.setSelectedItemIndex(static_cast<int>(m.curve), juce::dontSendNotification);
            r.invert.setToggleState(m.invert, juce::dontSendNotification);
        } else {
            r.source.setText(TRANS("Not mapped"), juce::dontSendNotification);
            r.minSlider.setValue(0.0, juce::dontSendNotification);
            r.maxSlider.setValue(100.0, juce::dontSendNotification);
            r.curve.setSelectedItemIndex(0, juce::dontSendNotification);
            r.invert.setToggleState(false, juce::dontSendNotification);
        }
        for (juce::Component* c :
             std::initializer_list<juce::Component*>{&r.minSlider, &r.maxSlider, &r.curve, &r.invert, &r.clear})
            c->setEnabled(bound);
    }
    oscToggle_.setToggleState(router_.oscEnabled(), juce::dontSendNotification);
    if (router_.oscEnabled()) lanToggle_.setToggleState(router_.oscAllowsNetwork(), juce::dontSendNotification);
    if (router_.oscEnabled()) oscPort_.setText(juce::String(router_.oscPort()), juce::dontSendNotification);
    oscPort_.setEnabled(!router_.oscEnabled());
}

void MacroMappingPanel::paint(juce::Graphics& g) { g.fillAll(KhensuTheme::bgBase); }

void MacroMappingPanel::resized()
{
    auto b = getLocalBounds().reduced(16);
    header_.setBounds(b.removeFromTop(26));
    b.removeFromTop(6);
    status_.setBounds(b.removeFromBottom(18));
    auto osc = b.removeFromBottom(26);
    oscToggle_.setBounds(osc.removeFromLeft(170));
    oscPort_.setBounds(osc.removeFromLeft(70).reduced(0, 2));
    osc.removeFromLeft(10);
    lanToggle_.setBounds(osc.removeFromLeft(220));
    oscNote_.setBounds(osc);
    b.removeFromBottom(8);
    const int rowH = juce::jmin(34, b.getHeight() / kRows);
    for (auto& r : rows_) {
        auto row = b.removeFromTop(rowH).reduced(0, 3);
        r.name.setBounds(row.removeFromLeft(90));
        r.learn.setBounds(row.removeFromLeft(64));
        row.removeFromLeft(6);
        r.clear.setBounds(row.removeFromRight(54));
        row.removeFromRight(6);
        r.invert.setBounds(row.removeFromRight(64));
        r.curve.setBounds(row.removeFromRight(110));
        row.removeFromRight(6);
        r.maxSlider.setBounds(row.removeFromRight(110));
        r.minSlider.setBounds(row.removeFromRight(110));
        row.removeFromRight(6);
        r.source.setBounds(row);
    }
}

} // namespace khensu
