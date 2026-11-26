// desktop-app/Source/ui/Copilot/HostedChainPanel.cpp
#include "HostedChainPanel.h"
#include "../../IAppController.h"
#include "../shared/RenderPaths.h"
#include <khensu/mixerchain/PluginScanner.h>

namespace khensu {

namespace {
constexpr const char* kChainPrefKey = "hosted_plugin_chain";
}

/// A floating, resizable window around a hosted plugin's own editor.
class HostedChainPanel::EditorWindow final : public juce::DocumentWindow {
  public:
    EditorWindow(juce::AudioPluginInstance& instance, std::function<void(EditorWindow*)> onClose)
        : juce::DocumentWindow(instance.getName(), KhensuTheme::bgBase, juce::DocumentWindow::closeButton),
          instance_(instance), onClose_(std::move(onClose))
    {
        if (auto* editor = instance.createEditorAndMakeActive())
            setContentOwned(editor, true);
        else
            setContentOwned(new juce::GenericAudioProcessorEditor(instance), true);
        setResizable(true, false);
        setUsingNativeTitleBar(true);
        centreWithSize(juce::jmax(320, getWidth()), juce::jmax(200, getHeight()));
        setVisible(true);
    }
    ~EditorWindow() override { clearContentComponent(); } // editor before instance

    void closeButtonPressed() override
    {
        if (onClose_) onClose_(this);
    }
    juce::AudioPluginInstance& instance() const noexcept { return instance_; }

  private:
    juce::AudioPluginInstance& instance_;
    std::function<void(EditorWindow*)> onClose_;
};

void HostedChainPanel::applyThemeColours()
{
    titleLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    list_.setColour(juce::ListBox::backgroundColourId, KhensuTheme::bgSurface);
    list_.setColour(juce::ListBox::outlineColourId, KhensuTheme::borderSubtle);
    for (auto* b : {&addButton_, &removeButton_, &upButton_, &downButton_, &bypassButton_, &editButton_, &midiButton_,
                    &renderButton_, &saveButton_, &restoreButton_}) {
        b->setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.15f));
        b->setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    }
}

HostedChainPanel::HostedChainPanel(IAppController& c) : controller_(c)
{
    applyThemeColours();
    setTitle(TRANS("Plugin host chain"));
    setFocusContainerType(juce::Component::FocusContainerType::focusContainer);

    titleLabel_.setText(TRANS("Host your own plugins"), juce::dontSendNotification);
    titleLabel_.setFont(KhensuTheme::uiFont(KhensuTheme::kFontH2, juce::Font::bold));
    addAndMakeVisible(titleLabel_);

    list_.setRowHeight(28);
    list_.setTitle(TRANS("Hosted plugin chain"));
    list_.setDescription(TRANS("Signal flows top to bottom. Return opens a plugin's editor, Delete removes it."));
    list_.setMultipleSelectionEnabled(false);
    addAndMakeVisible(list_);

    struct B {
        juce::TextButton* button;
        const char* tip;
        std::function<void()> action;
    };
    for (auto& b : std::vector<B>{
             {&addButton_, "Add a scanned plugin, or browse for a .vst3 file.", [this] { showAddMenu(); }},
             {&removeButton_, "Remove the selected plugin (Delete).", [this] { removeSelected(); }},
             {&upButton_, "Move the selected plugin earlier in the chain.", [this] { moveSelected(-1); }},
             {&downButton_, "Move the selected plugin later in the chain.", [this] { moveSelected(1); }},
             {&bypassButton_, "Bypass or re-enable the selected plugin.", [this] { toggleBypassSelected(); }},
             {&editButton_, "Open the selected plugin's own editor (Return).",
              [this] { openEditor(list_.getSelectedRow()); }},
             {&midiButton_, "Choose a MIDI file that plays instrument plugins during render.",
              [this] { chooseMidi(); }},
             {&renderButton_, "Render the session audio through the chain and load it as the A/B \"B\" side.",
              [this] { render(); }},
             {&saveButton_, "Save this chain (order, bypass, plugin settings).", [this] { saveChain(); }},
             {&restoreButton_, "Reload the last saved chain.", [this] { restoreChain(); }},
         }) {
        b.button->setTooltip(b.tip);
        b.button->onClick = b.action;
        addAndMakeVisible(*b.button);
    }

    statusLabel_.setFont(KhensuTheme::labelFont(type::caption));
    statusLabel_.setText(TRANS("Add plugins to build a chain. Renders are latency-compensated."),
                         juce::dontSendNotification);
    addAndMakeVisible(statusLabel_);
    refresh();
}

HostedChainPanel::~HostedChainPanel()
{
    // A render job reads chain_ on a pool thread: cancel it (renderFile polls
    // between 512-sample blocks) and wait for it to leave before chain_ dies.
    cancelRender_->store(true);
    renderIdle_->event.wait(-1);
    editors_.clear(); // editors must die before their instances
    list_.setModel(nullptr);
}

int HostedChainPanel::getNumRows() { return chain_.size(); }

juce::String HostedChainPanel::getNameForRow(int row)
{
    const auto s = chain_.slots();
    if (row < 0 || row >= static_cast<int>(s.size())) return {};
    const auto& slot = s[static_cast<size_t>(row)];
    return juce::String(row + 1) + ". " + slot.name + (slot.bypassed ? " (bypassed)" : "") +
           (slot.isInstrument ? ", instrument" : "");
}

void HostedChainPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    const auto s = chain_.slots();
    if (row < 0 || row >= static_cast<int>(s.size())) return;
    const auto& slot = s[static_cast<size_t>(row)];
    if (selected) g.fillAll(KhensuTheme::accentTeal.withAlpha(0.18f));
    auto r = juce::Rectangle<int>(0, 0, width, height).reduced(8, 0);
    g.setFont(KhensuTheme::bodyFont(type::secondary));
    g.setColour(slot.bypassed ? KhensuTheme::textTertiary : KhensuTheme::textPrimary);
    g.drawText(juce::String(row + 1) + ".  " + slot.name, r.removeFromLeft(width / 2), juce::Justification::centredLeft,
               true);
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.setColour(KhensuTheme::textSecondary);
    juce::String meta = slot.formatName + (slot.isInstrument ? "  instrument" : "  effect");
    if (slot.latencySamples > 0) meta << "  " << slot.latencySamples << " smp latency";
    if (slot.bypassed) meta << "  BYPASSED";
    g.drawText(meta, r, juce::Justification::centredRight, true);
}

void HostedChainPanel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) { openEditor(row); }
void HostedChainPanel::returnKeyPressed(int row) { openEditor(row); }
void HostedChainPanel::deleteKeyPressed(int) { removeSelected(); }

void HostedChainPanel::showAddMenu()
{
    juce::PopupMenu menu;
    std::vector<mixerchain::ScannedPlugin> scanned;
    if (auto listed = controller_.sessions().sessionManager().listScannedPlugins(); listed.is_ok()) scanned = listed.value();
    int id = 1;
    juce::PopupMenu effects, instruments;
    for (const auto& p : scanned) {
        if (p.file_path.empty()) {
            ++id;
            continue;
        }
        const juce::String label = juce::String(p.name) + (p.manufacturer.empty() ? "" : " - " + p.manufacturer);
        const bool inst = juce::String(p.category).containsIgnoreCase("instrument") ||
                          juce::String(p.category).containsIgnoreCase("synth");
        (inst ? instruments : effects).addItem(id++, label);
    }
    if (effects.getNumItems() > 0) menu.addSubMenu("Effects", effects);
    if (instruments.getNumItems() > 0) menu.addSubMenu("Instruments", instruments);
    if (scanned.empty()) menu.addItem(-2, "No scanned plugins - use Mixer Chain Generator > Scan My Plugins", false);
    menu.addSeparator();
    menu.addItem(-1, "Browse for a plugin file...");
    juce::Component::SafePointer<HostedChainPanel> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addButton_), [self, scanned](int result) {
        if (self == nullptr || result == 0) return;
        if (result == -1) {
            self->browseForPlugin();
            return;
        }
        if (result >= 1 && result <= static_cast<int>(scanned.size()))
            self->loadPluginFile(juce::File(juce::String(scanned[static_cast<size_t>(result - 1)].file_path)));
    });
}

void HostedChainPanel::browseForPlugin()
{
    chooser_ = std::make_unique<juce::FileChooser>(
        "Choose a plugin", juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory),
        "*.vst3;*.component");
    juce::Component::SafePointer<HostedChainPanel> self(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::canSelectDirectories,
                          [self](const juce::FileChooser& fc) {
                              if (self != nullptr && fc.getResult() != juce::File())
                                  self->loadPluginFile(fc.getResult());
                          });
}

void HostedChainPanel::loadPluginFile(const juce::File& file)
{
    setBusy(true, "Loading " + file.getFileNameWithoutExtension() + "...");
    juce::Component::SafePointer<HostedChainPanel> self(this);
    chain_.loadPluginAsync(file, [self](bool ok, juce::String message) {
        if (self == nullptr) return;
        self->setBusy(false, ok ? "Added " + message + "." : "Could not load plugin: " + message);
        self->refresh();
        if (ok) self->list_.selectRow(self->chain_.size() - 1);
    });
}

void HostedChainPanel::chooseMidi()
{
    chooser_ =
        std::make_unique<juce::FileChooser>("Choose a MIDI file for instrument plugins", juce::File(), "*.mid;*.midi");
    juce::Component::SafePointer<HostedChainPanel> self(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [self](const juce::FileChooser& fc) {
                              if (self == nullptr) return;
                              self->midiFile_ = fc.getResult();
                              self->statusLabel_.setText(self->midiFile_.existsAsFile()
                                                             ? "Instruments will play " + self->midiFile_.getFileName()
                                                             : juce::String("No MIDI file - instruments stay silent."),
                                                         juce::dontSendNotification);
                          });
}

void HostedChainPanel::closeEditorFor(juce::AudioPluginInstance* instance)
{
    editors_.erase(std::remove_if(editors_.begin(), editors_.end(),
                                  [instance](const auto& w) { return &w->instance() == instance; }),
                   editors_.end());
}

void HostedChainPanel::removeSelected()
{
    const int row = list_.getSelectedRow();
    if (busy_ || row < 0) return;
    closeEditorFor(chain_.instanceAt(row));
    chain_.remove(row);
    refresh();
    list_.selectRow(juce::jmin(row, chain_.size() - 1));
}

void HostedChainPanel::moveSelected(int delta)
{
    const int row = list_.getSelectedRow();
    if (busy_ || row < 0) return;
    if (chain_.move(row, row + delta)) {
        refresh();
        list_.selectRow(row + delta);
    }
}

void HostedChainPanel::toggleBypassSelected()
{
    const int row = list_.getSelectedRow();
    if (busy_ || row < 0) return;
    const auto s = chain_.slots();
    chain_.setBypassed(row, !s[static_cast<size_t>(row)].bypassed);
    refresh();
}

void HostedChainPanel::openEditor(int row)
{
    auto* inst = chain_.instanceAt(row);
    if (inst == nullptr) return;
    for (auto& w : editors_)
        if (&w->instance() == inst) {
            w->toFront(true);
            return;
        }
    juce::Component::SafePointer<HostedChainPanel> self(this);
    editors_.push_back(std::make_unique<EditorWindow>(*inst, [self](EditorWindow* w) {
        if (self == nullptr) return;
        // Deferred: the window is still inside its own closeButtonPressed().
        juce::MessageManager::callAsync([self, w] {
            if (self != nullptr) self->closeEditorFor(&w->instance());
        });
    }));
}

void HostedChainPanel::render()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || s->files.empty()) {
        statusLabel_.setText(TRANS("Load a session first - the chain renders its audio."), juce::dontSendNotification);
        return;
    }
    if (chain_.size() == 0) {
        statusLabel_.setText(TRANS("Add at least one plugin first."), juce::dontSendNotification);
        return;
    }
    // WF-1: render the current version (fixes stack) unless the user chose
    // the original; the output is named after the original either way.
    const juce::File source(controller_.renders().renderInputPath());
    const auto output = uniqueRenderFile(juce::File(juce::String(s->files[0].path)), "_hostedchain");
    const juce::String sessionId(s->id);
    const auto midi = midiFile_;
    setBusy(true, "Rendering through " + juce::String(chain_.size()) + " plugin(s)...");
    cancelRender_->store(false);
    juce::Component::SafePointer<HostedChainPanel> self(this);
    auto cancel = cancelRender_;
    auto idle = renderIdle_;
    idle->event.reset();
    // Signals `idle` when the job is destroyed too - a job the pool drops
    // unrun at shutdown must not leave the destructor waiting forever.
    struct SignalOnDestroy {
        std::shared_ptr<RenderIdle> idle;
        ~SignalOnDestroy() { idle->event.signal(); }
    };
    std::shared_ptr<SignalOnDestroy> idleGuard(new SignalOnDestroy{idle}); // no temporary: it would signal
    auto* chain = &chain_;
    auto* controllerPtr = &controller_;
    // The chain outlives the job: the destructor cancels and waits on
    // `idle` before chain_ is destroyed. A VST3 render of a full song takes
    // minutes: the long pool, never the short fetch pool (AUDIT-065).
    controller_.runLongInBackground([self, chain, source, output, midi, cancel, idle, idleGuard, controllerPtr,
                                     sessionId] {
        juce::ignoreUnused(idleGuard); // held only for its destructor
        DesktopPluginChain::RenderResult result;
        const auto shouldCancel = [cancel, controllerPtr] { return cancel->load() || controllerPtr->isShuttingDown(); };
        if (!shouldCancel()) result = chain->renderFile(source, output, shouldCancel, midi);
        idle->event.signal();
        juce::MessageManager::callAsync([self, result, controllerPtr, sessionId, source] {
            if (self == nullptr) return;
            if (!result.ok) {
                self->setBusy(false, "Render failed: " + result.error);
                return;
            }
            const auto* active = controllerPtr->getActiveSession();
            if (active == nullptr || juce::String(active->id) != sessionId) {
                self->setBusy(false, TRANS("Session changed - the render was kept on disk but not loaded."));
                return;
            }
            controllerPtr->renders().registerRenderOf(result.output.getFullPathName(), source.getFullPathName(),
                                                      sessionId); // AUDIT-002
            self->setBusy(false, "Rendered " + result.output.getFileName() + " (" +
                                     juce::String(result.compensatedLatencySamples) +
                                     " samples latency compensated) - use the transport A/B.");
        });
    });
}

void HostedChainPanel::saveChain()
{
    const auto xml = chain_.toValueTree().toXmlString();
    const auto json = juce::JSON::toString(juce::var(xml));
    const auto saved = controller_.sessions().sessionManager().setUserPreference(kChainPrefKey, json.toStdString());
    statusLabel_.setText(saved.is_ok() ? "Chain saved." : "Could not save the chain.", juce::dontSendNotification);
}

void HostedChainPanel::restoreChain()
{
    auto pref = controller_.sessions().sessionManager().getUserPreference(kChainPrefKey);
    if (!pref.is_ok() || pref.value().empty()) {
        statusLabel_.setText(TRANS("No saved chain yet."), juce::dontSendNotification);
        return;
    }
    const auto tree = juce::ValueTree::fromXml(juce::JSON::parse(juce::String(pref.value())).toString());
    if (!tree.isValid()) {
        statusLabel_.setText(TRANS("The saved chain is unreadable."), juce::dontSendNotification);
        return;
    }
    editors_.clear();
    setBusy(true, "Restoring chain...");
    juce::Component::SafePointer<HostedChainPanel> self(this);
    const auto expected = static_cast<int>(DesktopPluginChain::savedSlotPaths(tree).size());
    chain_.restoreAsync(tree, [self, expected](int loaded) {
        if (self == nullptr) return;
        self->setBusy(false, "Restored " + juce::String(loaded) + " of " + juce::String(expected) + " plugin(s).");
        self->refresh();
    });
}

void HostedChainPanel::refresh()
{
    list_.updateContent();
    list_.repaint();
    const bool hasRows = chain_.size() > 0;
    for (auto* b :
         {&removeButton_, &upButton_, &downButton_, &bypassButton_, &editButton_, &renderButton_, &saveButton_})
        b->setEnabled(hasRows && !busy_);
    addButton_.setEnabled(!busy_);
    restoreButton_.setEnabled(!busy_);
    midiButton_.setEnabled(!busy_);
}

void HostedChainPanel::setBusy(bool busy, const juce::String& status)
{
    busy_ = busy;
    statusLabel_.setText(status, juce::dontSendNotification);
    refresh();
}

void HostedChainPanel::paint(juce::Graphics& g) { g.fillAll(KhensuTheme::bgBase); }

void HostedChainPanel::resized()
{
    auto b = getLocalBounds().reduced(KhensuTheme::cardGap);
    titleLabel_.setBounds(b.removeFromTop(26));
    statusLabel_.setBounds(b.removeFromBottom(20));
    b.removeFromBottom(4);
    auto row2 = b.removeFromBottom(28);
    b.removeFromBottom(4);
    auto row1 = b.removeFromBottom(28);
    b.removeFromBottom(6);
    list_.setBounds(b);

    auto place = [](juce::Rectangle<int>& row, juce::Component& c, int w) {
        c.setBounds(row.removeFromLeft(w).reduced(2, 0));
    };
    place(row1, addButton_, 110);
    place(row1, removeButton_, 80);
    place(row1, upButton_, 85);
    place(row1, downButton_, 95);
    place(row1, bypassButton_, 75);
    place(row1, editButton_, 105);
    place(row2, midiButton_, 170);
    place(row2, renderButton_, 220);
    place(row2, saveButton_, 95);
    place(row2, restoreButton_, 110);
}

} // namespace khensu
