#include "HostedChainMiniPanel.h"
#include "../PluginProcessor.h"
#include <khensu/ui/KhensuPalette.h>
#include <algorithm>
#include <iterator>

namespace khensu {

namespace {
constexpr int kRefreshHz = 4;

juce::String latencyText(int samples) { return juce::String(juce::jmax(0, samples)) + " smp"; }
} // namespace

// ---------------------------------------------------------------------------
// Row
// ---------------------------------------------------------------------------

HostedChainMiniPanel::Row::Row(HostedChainMiniPanel& owner, int slotIndex, int numSlots, const SlotInfo& info)
{
    const auto displayName = info.name.isNotEmpty() ? info.name : juce::File(info.filePath).getFileNameWithoutExtension();
    const auto slotLabel = "Slot " + juce::String(slotIndex + 1) + " (" + displayName + ")";
    missing = info.missing;

    // A placeholder (plugin not found on this machine) keeps its saved state
    // in the project; it can only be removed (audit M5).
    nameLabel.setText(juce::String(slotIndex + 1) + ". " + (missing ? "missing: " + displayName : displayName),
                      juce::dontSendNotification);
    nameLabel.setFont(ui::Palette::uiFont(type::secondary, juce::Font::bold));
    nameLabel.setColour(juce::Label::textColourId, missing ? PluginLookAndFeel::statusBad : PluginLookAndFeel::textPrimary);
    nameLabel.setTooltip(missing ? "Not found: " + info.filePath
                                       + " - its saved settings are kept with the project. Remove it to drop them."
                                 : (info.filePath.isNotEmpty() ? info.filePath : displayName));
    nameLabel.setTitle(missing ? slotLabel + " missing" : slotLabel);
    addAndMakeVisible(nameLabel);

    latencyLabel.setFont(ui::Palette::uiFont(type::caption, juce::Font::plain));
    latencyLabel.setColour(juce::Label::textColourId, PluginLookAndFeel::textMuted);
    latencyLabel.setJustificationType(juce::Justification::centredRight);
    latencyLabel.setTitle(slotLabel + " latency");
    latencyLabel.setTooltip("Latency this plugin reports to the host (compensated automatically).");
    addAndMakeVisible(latencyLabel);

    bypassToggle.setComponentID("hostedBypass" + juce::String(slotIndex));
    bypassToggle.setTitle(slotLabel + " bypass");
    bypassToggle.setTooltip("Bypass " + displayName + " without unloading it.");
    bypassToggle.setColour(juce::ToggleButton::textColourId, PluginLookAndFeel::textMuted);
    bypassToggle.onClick = [&owner, slotIndex, this] { owner.setSlotBypass(slotIndex, bypassToggle.getToggleState()); };
    addAndMakeVisible(bypassToggle);

    wetSlider.setComponentID("hostedWet" + juce::String(slotIndex));
    wetSlider.setRange(0.0, 100.0, 1.0);
    wetSlider.setTextValueSuffix(" %");
    wetSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 20);
    wetSlider.setColour(juce::Slider::trackColourId, PluginLookAndFeel::accentTeal);
    wetSlider.setColour(juce::Slider::backgroundColourId, PluginLookAndFeel::controlOutline);
    wetSlider.setColour(juce::Slider::textBoxTextColourId, PluginLookAndFeel::textPrimary);
    wetSlider.setColour(juce::Slider::textBoxOutlineColourId, PluginLookAndFeel::controlOutline);
    wetSlider.setTitle(slotLabel + " wet mix");
    wetSlider.setTooltip("Wet/dry mix for " + displayName
                         + " - below 100% the dry side is latency-aligned to the plugin.");
    wetSlider.onValueChange = [&owner, slotIndex, this] {
        owner.setSlotWetMix(slotIndex, static_cast<float>(wetSlider.getValue() / 100.0));
    };
    addAndMakeVisible(wetSlider);

    removeButton.setComponentID("hostedRemove" + juce::String(slotIndex));
    removeButton.setTitle("Remove " + slotLabel);
    removeButton.setTooltip("Unload " + displayName + " from the hosted chain.");
    removeButton.setColour(juce::TextButton::buttonColourId, PluginLookAndFeel::bgDark);
    removeButton.setColour(juce::TextButton::textColourOffId, PluginLookAndFeel::statusBad);
    removeButton.onClick = [&owner, slotIndex] { owner.removeSlot(slotIndex); };
    addAndMakeVisible(removeButton);

    // Hosted plugin editor (PLG-06): opens the plugin's own UI in a window.
    editButton.setComponentID("hostedEdit" + juce::String(slotIndex));
    editButton.setTitle("Edit " + slotLabel);
    editButton.setTooltip("Open " + displayName + "'s own editor in a window.");
    editButton.setColour(juce::TextButton::buttonColourId, PluginLookAndFeel::bgDark);
    editButton.setColour(juce::TextButton::textColourOffId, PluginLookAndFeel::accentTeal);
    editButton.onClick = [&owner, slotIndex] { owner.editSlot(slotIndex); };
    addAndMakeVisible(editButton);

    // Reorder (PLG-06).
    upButton.setComponentID("hostedUp" + juce::String(slotIndex));
    upButton.setTitle("Move " + slotLabel + " up");
    upButton.setTooltip("Process " + displayName + " earlier in the chain.");
    upButton.onClick = [&owner, slotIndex] { owner.moveSlot(slotIndex, -1); };
    downButton.setComponentID("hostedDown" + juce::String(slotIndex));
    downButton.setTitle("Move " + slotLabel + " down");
    downButton.setTooltip("Process " + displayName + " later in the chain.");
    downButton.onClick = [&owner, slotIndex] { owner.moveSlot(slotIndex, 1); };
    for (auto* b : {&upButton, &downButton}) {
        b->setColour(juce::TextButton::buttonColourId, PluginLookAndFeel::bgDark);
        b->setColour(juce::TextButton::textColourOffId, PluginLookAndFeel::textMuted);
        addAndMakeVisible(*b);
    }
    upButton.setEnabled(slotIndex > 0);
    downButton.setEnabled(slotIndex < numSlots - 1);

    bypassToggle.setEnabled(!missing);
    wetSlider.setEnabled(!missing);
    editButton.setEnabled(!missing);

    sync(info);
}

void HostedChainMiniPanel::Row::sync(const SlotInfo& info)
{
    bypassToggle.setToggleState(info.bypassed, juce::dontSendNotification);
    // Don't fight the user mid-drag; the next refresh after release catches up.
    if (!wetSlider.isMouseButtonDown())
        wetSlider.setValue(juce::roundToInt(info.wetMix * 100.0f), juce::dontSendNotification);
    latencyLabel.setText(info.missing ? juce::String("--") : latencyText(info.latencySamples),
                         juce::dontSendNotification);
    const auto alpha = info.bypassed || info.missing ? 0.5f : 1.0f;
    nameLabel.setAlpha(alpha);
    wetSlider.setAlpha(alpha);
}

void HostedChainMiniPanel::Row::resized()
{
    auto b = getLocalBounds().reduced(4, 3);
    upButton.setBounds(b.removeFromLeft(22));
    downButton.setBounds(b.removeFromLeft(22));
    b.removeFromLeft(4);
    removeButton.setBounds(b.removeFromRight(64));
    b.removeFromRight(4);
    editButton.setBounds(b.removeFromRight(44));
    b.removeFromRight(6);
    wetSlider.setBounds(b.removeFromRight(juce::jmin(200, juce::jmax(90, b.getWidth() / 3))));
    b.removeFromRight(6);
    bypassToggle.setBounds(b.removeFromRight(76));
    b.removeFromRight(4);
    latencyLabel.setBounds(b.removeFromRight(64));
    nameLabel.setBounds(b);
}

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------

HostedChainMiniPanel::HostedChainMiniPanel(KhensuCopilotProcessor& processor) : processor_(processor)
{
    setLookAndFeel(&lookAndFeel_);
    setTitle("Hosted plugin chain");

    headerLabel_.setText("Hosted Plugins", juce::dontSendNotification);
    headerLabel_.setFont(ui::Palette::uiFont(type::body, juce::Font::bold));
    headerLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::textPrimary);
    headerLabel_.setAccessible(false);
    addAndMakeVisible(headerLabel_);

    addButton_.setComponentID("hostedAddButton");
    addButton_.setTitle("Add plugin");
    addButton_.setTooltip("Load a VST3 plugin into the hosted chain (processed after the corrective DSP, "
                          "before the output limiter). Khensu Copilot itself cannot be hosted.");
    addButton_.setColour(juce::TextButton::buttonColourId, PluginLookAndFeel::bgDark);
    addButton_.setColour(juce::TextButton::textColourOffId, PluginLookAndFeel::accentTeal);
    addButton_.onClick = [this] { launchAddChooser(); };
    addAndMakeVisible(addButton_);

    statusLabel_.setFont(ui::Palette::uiFont(type::caption, juce::Font::plain));
    statusLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::textMuted);
    statusLabel_.setTitle("Hosted chain status");
    statusLabel_.setTooltip("Result of the last hosted-chain action.");
    addAndMakeVisible(statusLabel_);

    emptyLabel_.setText("No hosted plugins. Use \"Add plugin...\" to load a VST3.", juce::dontSendNotification);
    emptyLabel_.setFont(ui::Palette::uiFont(type::secondary, juce::Font::plain));
    emptyLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::textMuted);
    emptyLabel_.setJustificationType(juce::Justification::centred);
    emptyLabel_.setTitle("Hosted chain is empty");
    addAndMakeVisible(emptyLabel_);

    viewport_.setViewedComponent(&rowsContent_, false);
    viewport_.setScrollBarsShown(true, false);
    viewport_.setTitle("Hosted plugin slots");
    addAndMakeVisible(viewport_);

    refreshFromProcessor();
    visibilityChanged();
}

void HostedChainMiniPanel::visibilityChanged()
{
    // Poll the processor only while the Hosted tab is shown.
    if (isVisible()) {
        refreshFromProcessor();
        startTimerHz(kRefreshHz);
    } else {
        stopTimer();
    }
}

HostedChainMiniPanel::~HostedChainMiniPanel()
{
    stopTimer();
    setLookAndFeel(nullptr);
}

void HostedChainMiniPanel::paint(juce::Graphics& g)
{
    g.fillAll(PluginLookAndFeel::bgPanel);
    g.setColour(PluginLookAndFeel::controlOutline);
    g.drawRect(getLocalBounds(), 1);

    if (!rows_.empty()) g.drawRect(viewport_.getBounds(), 1); // frame the slot list
}

void HostedChainMiniPanel::resized()
{
    auto b = getLocalBounds().reduced(8);
    auto header = b.removeFromTop(26);
    addButton_.setBounds(header.removeFromRight(120));
    header.removeFromRight(8);
    headerLabel_.setBounds(header);
    b.removeFromTop(4);

    statusLabel_.setBounds(b.removeFromBottom(20));
    b.removeFromBottom(4);

    viewport_.setBounds(b);
    emptyLabel_.setBounds(b);
    layoutRows();
}

void HostedChainMiniPanel::layoutRows()
{
    const int width = juce::jmax(0, viewport_.getMaximumVisibleWidth());
    rowsContent_.setSize(width, kRowHeight * static_cast<int>(rows_.size()));
    for (size_t i = 0; i < rows_.size(); ++i)
        rows_[i]->setBounds(0, static_cast<int>(i) * kRowHeight, width, kRowHeight);
}

void HostedChainMiniPanel::refreshFromProcessor()
{
    const auto slots = processor_.getHostedPluginSlots();
    if (hasSnapshot_ && slots == lastSlots_) return;

    bool structural = !hasSnapshot_ || slots.size() != lastSlots_.size() || slots.size() != rows_.size();
    for (size_t i = 0; !structural && i < slots.size(); ++i)
        structural = slots[i].id != lastSlots_[i].id || slots[i].missing != lastSlots_[i].missing
                     || slots[i].name != lastSlots_[i].name || slots[i].filePath != lastSlots_[i].filePath;

    if (structural)
        rebuildRows(slots);
    else
        for (size_t i = 0; i < slots.size(); ++i)
            rows_[i]->sync(slots[i]);

    lastSlots_ = slots;
    hasSnapshot_ = true;
    if (statusLabel_.getText().isEmpty() || (structural && pendingLoads_ == 0))
        setStatus(slots.empty() ? juce::String("Chain empty")
                                : juce::String(slots.size()) + (slots.size() == 1 ? " plugin" : " plugins")
                                      + " hosted",
                  PluginLookAndFeel::textMuted);
}

void HostedChainMiniPanel::rebuildRows(const std::vector<SlotInfo>& slots)
{
    rows_.clear(); // Row destructors remove themselves from rowsContent_
    for (size_t i = 0; i < slots.size(); ++i) {
        auto row = std::make_unique<Row>(*this, static_cast<int>(i), static_cast<int>(slots.size()), slots[i]);
        rowsContent_.addAndMakeVisible(*row);
        rows_.push_back(std::move(row));
    }
    emptyLabel_.setVisible(rows_.empty());
    viewport_.setVisible(!rows_.empty());
    layoutRows();
    repaint();
}

void HostedChainMiniPanel::setStatus(const juce::String& text, juce::Colour colour)
{
    statusLabel_.setText(text, juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId, colour);
}

void HostedChainMiniPanel::launchAddChooser()
{
    juce::File startDir;
#if JUCE_PLUGINHOST_VST3
    const auto searchPath = juce::VST3PluginFormat().getDefaultLocationsToSearch();
    for (int i = 0; i < searchPath.getNumPaths(); ++i)
        if (searchPath[i].isDirectory()) {
            startDir = searchPath[i];
            break;
        }
#endif
    chooser_ = std::make_unique<juce::FileChooser>("Add a VST3 plugin to the hosted chain", startDir, "*.vst3");
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    juce::Component::SafePointer<HostedChainMiniPanel> safeThis(this);
    chooser_->launchAsync(flags, [safeThis](const juce::FileChooser& chooser) {
        auto* self = safeThis.getComponent();
        if (self == nullptr) return;
        const auto file = chooser.getResult();
        if (file != juce::File()) self->loadPluginFile(file);
    });
}

void HostedChainMiniPanel::loadPluginFile(const juce::File& file)
{
    const auto fileName = file.getFileName();
    ++pendingLoads_;
    addButton_.setEnabled(false);
    setStatus("Loading " + fileName + "...", PluginLookAndFeel::accentTeal);

    juce::Component::SafePointer<HostedChainMiniPanel> safeThis(this);
    processor_.addPluginToChainAsync(file.getFullPathName(), [safeThis, fileName](bool ok) {
        // addPluginToChainAsync completes on the message thread.
        auto* self = safeThis.getComponent();
        if (self == nullptr) return;
        self->pendingLoads_ = juce::jmax(0, self->pendingLoads_ - 1);
        self->addButton_.setEnabled(self->pendingLoads_ == 0);
        self->refreshFromProcessor();
        if (ok)
            self->setStatus("Added " + fileName, PluginLookAndFeel::statusGood);
        else
            self->setStatus("Could not load " + fileName + " - not a loadable VST3?", PluginLookAndFeel::statusBad);
    });
}

void HostedChainMiniPanel::setSlotBypass(int slotIndex, bool bypassed)
{
    processor_.setHostedPluginBypass(slotIndex, bypassed);
    refreshFromProcessor();
}

void HostedChainMiniPanel::setSlotWetMix(int slotIndex, float wetMix)
{
    processor_.setHostedPluginWetMix(slotIndex, wetMix);
}

void HostedChainMiniPanel::editSlot(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= static_cast<int>(lastSlots_.size())) return;
    const auto expected = lastSlots_[static_cast<size_t>(slotIndex)];
    juce::Component::SafePointer<HostedChainMiniPanel> safeThis(this);
    juce::MessageManager::callAsync([safeThis, expected] {
        auto* self = safeThis.getComponent();
        if (self == nullptr) return;
        const auto current = self->processor_.getHostedPluginSlots();
        const auto it = std::find_if(current.begin(), current.end(),
                                     [&expected](const SlotInfo& slot) { return slot.id == expected.id; });
        if (it == current.end()) return;
        if (!self->processor_.openHostedPluginEditor(static_cast<int>(std::distance(current.begin(), it))))
            self->setStatus("No editor for " + expected.name, PluginLookAndFeel::statusWarn);
    });
}

void HostedChainMiniPanel::moveSlot(int slotIndex, int delta)
{
    if (slotIndex < 0 || slotIndex >= static_cast<int>(lastSlots_.size())) return;
    const auto expected = lastSlots_[static_cast<size_t>(slotIndex)];
    // Deferred like removeSlot(): the move rebuilds the row that was clicked.
    juce::Component::SafePointer<HostedChainMiniPanel> safeThis(this);
    juce::MessageManager::callAsync([safeThis, expected, delta] {
        auto* self = safeThis.getComponent();
        if (self == nullptr) return;
        const auto current = self->processor_.getHostedPluginSlots();
        const auto it = std::find_if(current.begin(), current.end(),
                                     [&expected](const SlotInfo& slot) { return slot.id == expected.id; });
        if (it == current.end()) return;
        const int from = static_cast<int>(std::distance(current.begin(), it));
        if (self->processor_.moveHostedPlugin(from, from + delta)) {
            self->refreshFromProcessor();
            self->setStatus("Moved " + expected.name + (delta < 0 ? " up" : " down"), PluginLookAndFeel::textMuted);
        }
    });
}

void HostedChainMiniPanel::removeSlot(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= static_cast<int>(lastSlots_.size())) return;
    const auto expected = lastSlots_[static_cast<size_t>(slotIndex)];
    // The click came from a Row this rebuild destroys - defer past its
    // onClick so the button isn't deleted while still on the call stack.
    juce::Component::SafePointer<HostedChainMiniPanel> safeThis(this);
    juce::MessageManager::callAsync([safeThis, expected] {
        auto* self = safeThis.getComponent();
        if (self == nullptr) return;
        // A double-click queues two removals; remove by the clicked slot's
        // unique id, never by index or name+path - two copies of the same
        // plugin share those, so both used to go (audit L7).
        const auto current = self->processor_.getHostedPluginSlots();
        const auto it = std::find_if(current.begin(), current.end(),
                                     [&expected](const SlotInfo& slot) { return slot.id == expected.id; });
        if (it == current.end()) return;

        self->processor_.removePluginFromChain(static_cast<int>(std::distance(current.begin(), it)));
        self->refreshFromProcessor();
        self->setStatus(expected.name.isNotEmpty() ? "Removed " + expected.name : juce::String("Removed plugin"),
                        PluginLookAndFeel::textMuted);
    });
}

} // namespace khensu
