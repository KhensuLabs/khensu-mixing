// plugin-copilot/Source/ui/PresetBar.cpp
#include "PresetBar.h"
#include "PluginLookAndFeel.h"
#include "../CopilotPresets.h"
#include "../PluginProcessor.h"

namespace khensu {

PresetBar::PresetBar(KhensuCopilotProcessor& processor) : processor_(processor)
{
    setTitle(TRANS("Presets"));
    setFocusContainerType(juce::Component::FocusContainerType::focusContainer);

    prevButton_.setTitle(TRANS("Previous preset"));
    prevButton_.setTooltip(TRANS("Load the previous preset"));
    prevButton_.onClick = [this] { step(-1); };
    nextButton_.setTitle(TRANS("Next preset"));
    nextButton_.setTooltip(TRANS("Load the next preset"));
    nextButton_.onClick = [this] { step(1); };
    nameButton_.setComponentID("presetName");
    nameButton_.setTitle(TRANS("Preset"));
    nameButton_.setTooltip(TRANS("Presets: save, save as, delete, factory and your own"));
    nameButton_.onClick = [this] { showMenu(); };

    aButton_.setComponentID("snapshotA");
    bButton_.setComponentID("snapshotB");
    aButton_.setTitle(TRANS("Snapshot A"));
    bButton_.setTitle(TRANS("Snapshot B"));
    aButton_.setTooltip(TRANS("Snapshot A of every DSP parameter - switching keeps the other side as it was"));
    bButton_.setTooltip(TRANS("Snapshot B of every DSP parameter - switching keeps the other side as it was"));
    for (auto* b : {&aButton_, &bButton_}) {
        b->setClickingTogglesState(false);
        b->setColour(juce::TextButton::buttonOnColourId, PluginLookAndFeel::accentTeal.withAlpha(0.22f));
    }
    aButton_.onClick = [this] {
        processor_.switchSnapshot(0);
        refresh();
    };
    bButton_.onClick = [this] {
        processor_.switchSnapshot(1);
        refresh();
    };
    copyButton_.setComponentID("snapshotCopy");
    copyButton_.setTitle(TRANS("Copy to the other snapshot"));
    copyButton_.onClick = [this] { processor_.copyActiveSnapshotToOther(); };

    // UX2-6b: undo history (Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y in the editor).
    undoButton_.setComponentID("undoButton");
    redoButton_.setComponentID("redoButton");
    historyButton_.setComponentID("historyButton");
    undoButton_.setTitle(TRANS("Undo"));
    redoButton_.setTitle(TRANS("Redo"));
    historyButton_.setButtonText(TRANS("History"));
    historyButton_.setTitle(TRANS("Undo history"));
    historyButton_.setTooltip(TRANS("The last 20 changes - pick one to go back (or forward) to it"));
    undoButton_.onClick = [this] {
        processor_.undoHistory().undo();
        refresh();
    };
    redoButton_.onClick = [this] {
        processor_.undoHistory().redo();
        refresh();
    };
    historyButton_.onClick = [this] { showHistoryMenu(); };

    int order = 1;
    for (juce::Button* b : {static_cast<juce::Button*>(&prevButton_), static_cast<juce::Button*>(&nameButton_),
                            static_cast<juce::Button*>(&nextButton_), static_cast<juce::Button*>(&aButton_),
                            static_cast<juce::Button*>(&bButton_), static_cast<juce::Button*>(&copyButton_),
                            static_cast<juce::Button*>(&undoButton_), static_cast<juce::Button*>(&redoButton_),
                            static_cast<juce::Button*>(&historyButton_)}) {
        b->setWantsKeyboardFocus(true);
        b->setExplicitFocusOrder(order++);
        addAndMakeVisible(*b);
    }
    refresh();
}

PresetBar::~PresetBar()
{
    if (saveDialog_ != nullptr) saveDialog_->exitModalState(0);
}

std::vector<PresetBar::Entry> PresetBar::entries() const
{
    std::vector<Entry> list;
    for (int i = 0; i < processor_.getNumPrograms(); ++i) list.push_back({true, i, {}, processor_.getProgramName(i)});
    // Re-read every time (the desktop may have added or removed presets).
    for (const auto& listed : processor_.listUserPresets())
        list.push_back({false, 0, listed.file, listed.preset.name});
    return list;
}

juce::File PresetBar::currentUserFile() const
{
    const auto name = processor_.getCurrentPresetName();
    if (name.isEmpty()) return {};
    for (const auto& e : entries())
        if (!e.factory && e.name == name) return e.file;
    return {};
}

void PresetBar::step(int direction)
{
    const auto list = entries();
    if (list.empty()) return;
    const auto name = processor_.getCurrentPresetName();
    int index = -1;
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].name == name) index = static_cast<int>(i);
    const int n = static_cast<int>(list.size());
    const int next = index < 0 ? (direction > 0 ? 0 : n - 1) : ((index + direction) % n + n) % n;
    const auto& e = list[static_cast<size_t>(next)];
    if (e.factory) {
        processor_.loadProgramFromUi(e.program);
    } else {
        juce::String error;
        processor_.loadUserPreset(e.file, error);
    }
    refresh();
}

void PresetBar::showMenu()
{
    const auto list = entries();
    const auto current = processor_.getCurrentPresetName();
    const auto userFile = currentUserFile();
    juce::PopupMenu menu;
    menu.addItem(1, TRANS("Save"), userFile.existsAsFile());
    menu.addItem(2, TRANS("Save as..."));
    menu.addItem(3, TRANS("Delete"), userFile.existsAsFile());
    menu.addSeparator();
    menu.addSectionHeader(TRANS("Factory"));
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].factory) menu.addItem(100 + static_cast<int>(i), list[i].name, true, list[i].name == current);
    bool anyUser = false;
    for (size_t i = 0; i < list.size(); ++i)
        if (!list[i].factory) {
            if (!anyUser) menu.addSectionHeader(TRANS("Your presets"));
            anyUser = true;
            menu.addItem(100 + static_cast<int>(i), list[i].name, true, list[i].file == userFile);
        }
    juce::Component::SafePointer<PresetBar> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&nameButton_), [safeThis, list](int choice) {
        auto* self = safeThis.getComponent();
        if (self == nullptr || choice <= 0) return;
        if (choice == 1) {
            juce::String error;
            self->processor_.saveUserPreset(self->processor_.getCurrentPresetName(), error);
        } else if (choice == 2) {
            self->saveAs();
        } else if (choice == 3) {
            self->deleteCurrent();
        } else if (choice >= 100 && choice - 100 < static_cast<int>(list.size())) {
            const auto& e = list[static_cast<size_t>(choice - 100)];
            if (e.factory)
                self->processor_.loadProgramFromUi(e.program);
            else {
                juce::String error;
                self->processor_.loadUserPreset(e.file, error);
            }
        }
        self->refresh();
    });
}

void PresetBar::saveAs()
{
    saveDialog_ = std::make_unique<juce::AlertWindow>(TRANS("Save preset"), TRANS("Name the preset:"),
                                                      juce::MessageBoxIconType::NoIcon, this);
    auto current = processor_.getCurrentPresetName();
    saveDialog_->addTextEditor("name", current.isNotEmpty() ? current : TRANS("My preset"));
    saveDialog_->addButton(TRANS("Save"), 1, juce::KeyPress(juce::KeyPress::returnKey));
    saveDialog_->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<PresetBar> safeThis(this);
    saveDialog_->enterModalState(true, juce::ModalCallbackFunction::create([safeThis](int result) {
                                     auto* self = safeThis.getComponent();
                                     if (self == nullptr || self->saveDialog_ == nullptr) return;
                                     const auto name = self->saveDialog_->getTextEditorContents("name");
                                     self->saveDialog_.reset();
                                     if (result != 1) return;
                                     juce::String error;
                                     self->processor_.saveUserPreset(name, error);
                                     self->refresh();
                                 }),
                                 false);
}

void PresetBar::deleteCurrent()
{
    const auto file = currentUserFile();
    if (!file.existsAsFile()) return;
    juce::Component::SafePointer<PresetBar> safeThis(this);
    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                     .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                     .withTitle(TRANS("Delete preset"))
                                     .withMessage(TRANS("Delete the preset") + " \"" + file.getFileNameWithoutExtension()
                                                  + "\"?")
                                     .withButton(TRANS("Delete"))
                                     .withButton(TRANS("Cancel"))
                                     .withAssociatedComponent(this),
                                 [safeThis, file](int result) {
                                     if (result != 1) return;
                                     file.deleteFile();
                                     if (auto* self = safeThis.getComponent()) self->refresh();
                                 });
}

juce::PopupMenu PresetBar::buildHistoryMenu() const
{
    // Ids: 1000 + n undoes n + 1 steps, 2000 + n redoes n + 1, 3000 clears.
    auto& history = processor_.undoHistory();
    const auto undos = history.undoNames(20);
    const auto redos = history.redoNames(20);
    juce::PopupMenu menu;
    if (undos.isEmpty() && redos.isEmpty()) {
        menu.addItem(1, TRANS("No changes yet"), false);
        return menu;
    }
    if (!redos.isEmpty()) {
        menu.addSectionHeader(TRANS("Redo"));
        for (int i = redos.size() - 1; i >= 0; --i) menu.addItem(2000 + i, redos[i]);
    }
    if (!undos.isEmpty()) {
        menu.addSectionHeader(TRANS("Undo"));
        for (int i = 0; i < undos.size(); ++i) menu.addItem(1000 + i, undos[i], true, i == 0);
    }
    menu.addSeparator();
    menu.addItem(3000, TRANS("Clear history"));
    return menu;
}

juce::StringArray PresetBar::historyMenuTextForTesting() const
{
    juce::StringArray items;
    const auto menu = buildHistoryMenu(); // the iterator keeps a reference
    for (juce::PopupMenu::MenuItemIterator it(menu); it.next();) {
        const auto& item = it.getItem();
        if (item.isSectionHeader || item.isSeparator) continue;
        items.add(item.text);
    }
    return items;
}

void PresetBar::pickHistoryItem(int itemId)
{
    auto& history = processor_.undoHistory();
    if (itemId >= 1000 && itemId < 2000)
        history.undoSteps(itemId - 1000 + 1);
    else if (itemId >= 2000 && itemId < 3000)
        history.redoSteps(itemId - 2000 + 1);
    else if (itemId == 3000)
        history.clear();
    refresh();
}

void PresetBar::showHistoryMenu()
{
    juce::Component::SafePointer<PresetBar> safeThis(this);
    buildHistoryMenu().showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&historyButton_),
                                     [safeThis](int choice) {
                                         if (auto* self = safeThis.getComponent(); self != nullptr && choice > 0)
                                             self->pickHistoryItem(choice);
                                     });
}

void PresetBar::ArrowButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    auto area = getLocalBounds().toFloat().reduced(1.0f);
    const bool enabled = isEnabled();
    g.setColour(PluginLookAndFeel::controlOutline.withAlpha(enabled ? 1.0f : 0.5f));
    g.drawRoundedRectangle(area.reduced(0.5f), 4.0f, 1.0f);
    if (enabled && (highlighted || down)) {
        g.setColour(PluginLookAndFeel::textPrimary.withAlpha(down ? 0.14f : 0.07f));
        g.fillRoundedRectangle(area, 4.0f);
    }
    // A hooked arrow: a curve from bottom right over the top, ending in a
    // head pointing left (undo) - mirrored for redo.
    const auto c = area.getCentre();
    const float r = juce::jmin(area.getWidth(), area.getHeight()) * 0.24f;
    const float dir = redo_ ? -1.0f : 1.0f;
    juce::Path arrow;
    const float y = c.y;
    arrow.startNewSubPath(c.x + dir * r, y + r * 0.6f);
    arrow.cubicTo(c.x + dir * r * 1.2f, y - r * 0.9f, c.x - dir * r * 0.2f, y - r * 1.1f, c.x - dir * r, y - r * 0.15f);
    const juce::Point<float> end(c.x - dir * r, y - r * 0.15f);
    juce::Path tri;
    tri.addTriangle(end.translated(-dir * r * 0.15f, -r * 0.85f), end.translated(-dir * r * 0.2f, r * 0.35f),
                    end.translated(dir * r * 0.75f, r * 0.05f));
    g.setColour((enabled ? PluginLookAndFeel::textPrimary : PluginLookAndFeel::textMuted).withAlpha(enabled ? 0.9f : 0.45f));
    g.strokePath(arrow, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.fillPath(tri);
}

void PresetBar::refresh()
{
    auto name = processor_.getCurrentPresetName();
    if (name.isEmpty()) name = TRANS("No preset");
    if (nameButton_.getButtonText() != name) nameButton_.setButtonText(name);
    const int active = processor_.getActiveSnapshot();
    aButton_.setToggleState(active == 0, juce::dontSendNotification);
    bButton_.setToggleState(active == 1, juce::dontSendNotification);
    // UX2-6b: what Undo / Redo would do, in their tooltips.
    auto& history = processor_.undoHistory();
    history.serviceRequests(); // a host state restore clears it
    if (history.revision() != lastHistoryRevision_) {
        lastHistoryRevision_ = history.revision();
        const auto undos = history.undoNames(1);
        const auto redos = history.redoNames(1);
        undoButton_.setEnabled(!undos.isEmpty());
        redoButton_.setEnabled(!redos.isEmpty());
        undoButton_.setTooltip(undos.isEmpty() ? TRANS("Nothing to undo (Ctrl+Z)")
                                               : TRANS("Undo") + " " + undos[0] + " (Ctrl+Z)");
        redoButton_.setTooltip(redos.isEmpty() ? TRANS("Nothing to redo (Ctrl+Shift+Z / Ctrl+Y)")
                                               : TRANS("Redo") + " " + redos[0] + " (Ctrl+Shift+Z)");
    }
    const juce::String copyText = active == 0 ? "A > B" : "B > A";
    if (copyButton_.getButtonText() != copyText) {
        copyButton_.setButtonText(copyText);
        copyButton_.setTooltip(active == 0 ? TRANS("Copy snapshot A over B") : TRANS("Copy snapshot B over A"));
    }
}

void PresetBar::resized()
{
    auto b = getLocalBounds();
    const int h = b.getHeight();
    // Undo / Redo / History on the right, after the A/B group.
    historyButton_.setBounds(b.removeFromRight(64));
    b.removeFromRight(2);
    redoButton_.setBounds(b.removeFromRight(26));
    b.removeFromRight(2);
    undoButton_.setBounds(b.removeFromRight(26));
    b.removeFromRight(8);
    prevButton_.setBounds(b.removeFromLeft(24).withSizeKeepingCentre(24, h));
    b.removeFromLeft(2);
    const int abW = 26, copyW = 50;
    copyButton_.setBounds(b.removeFromRight(copyW));
    b.removeFromRight(4);
    bButton_.setBounds(b.removeFromRight(abW));
    aButton_.setBounds(b.removeFromRight(abW));
    b.removeFromRight(8);
    nextButton_.setBounds(b.removeFromRight(24));
    b.removeFromRight(2);
    nameButton_.setBounds(b);
}

} // namespace khensu
