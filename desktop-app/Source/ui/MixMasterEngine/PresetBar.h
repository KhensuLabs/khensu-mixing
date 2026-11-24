// desktop-app/Source/ui/MixMasterEngine/PresetBar.h
// UI-9: the plugin-style preset bar on Mix / Master -
//   [<] [ Preset name ] [>] [Apply] [...]   Rack  [A][B]
// Arrows step through the saved and factory presets; Apply renders the shown
// preset's desktop stages as a new version (WF-1) and stages its rack
// settings in the one plugin chain (WF-9) for the Signal Flow Rack to apply;
// the menu saves the rack's settings with the last applied chain (Save over
// the shown preset, Save as... a new one), deletes, or picks a factory chain.
// A | B are two snapshots of the Signal Flow Rack: switching keeps the rack's
// current settings in the slot being left and recalls the other.
// Backed by the shared chain presets (PresetStore.h, FN2-5): the plugin's
// preset menu lists the same files.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <array>
#include <map>
#include "../../IAppController.h"
#include "../../RenderJobManager.h"
#include "../shared/ButtonRole.h"
#include "../shared/KhensuTheme.h"
#include "PresetStore.h"

namespace khensu {

class PresetBar final : public juce::Component {
  public:
    explicit PresetBar(IAppController& controller) : controller_(controller)
    {
        setTitle(TRANS("Presets"));
        prevButton_.setTitle(TRANS("Previous preset"));
        prevButton_.setTooltip(TRANS("Previous preset"));
        nextButton_.setTitle(TRANS("Next preset"));
        nextButton_.setTooltip(TRANS("Next preset"));
        menuButton_.setTitle(TRANS("Preset menu"));
        menuButton_.setTooltip(TRANS("Save, save as, delete, or start from a factory preset"));
        applyButton_.setTooltip(TRANS("Apply the shown preset: its stages render as a new version (level-matched "
                                      "for A/B), its rack settings are staged on the Signal Flow Rack"));
        nameLabel_.setTitle(TRANS("Current preset"));
        nameLabel_.setJustificationType(juce::Justification::centred);
        nameLabel_.setFont(KhensuTheme::bodyFont());
        snapA_.setTooltip(TRANS("Rack snapshot A: switch to keep the rack's settings here and recall B's"));
        snapB_.setTooltip(TRANS("Rack snapshot B: switch to keep the rack's settings here and recall A's"));
        for (auto* b : {&snapA_, &snapB_}) {
            b->setClickingTogglesState(false);
            addAndMakeVisible(b);
        }
        snapA_.setToggleState(true, juce::dontSendNotification);
        prevButton_.onClick = [this] { step(-1); };
        nextButton_.onClick = [this] { step(+1); };
        applyButton_.onClick = [this] { applyShown(); };
        menuButton_.onClick = [this] { showMenu(); };
        snapA_.onClick = [this] { switchSnapshot(0); };
        snapB_.onClick = [this] { switchSnapshot(1); };
        for (juce::Component* c : {(juce::Component*)&prevButton_, (juce::Component*)&nameLabel_,
                                   (juce::Component*)&nextButton_, (juce::Component*)&applyButton_,
                                   (juce::Component*)&menuButton_})
            addAndMakeVisible(c);
        applyThemeColours();
        refresh();
    }

    /// Re-reads the store (another surface may have saved or deleted).
    void refresh()
    {
        entries_ = preset_store::list();
        for (const auto& [name, chain] : preset_store::factory()) {
            preset_store::Entry e;
            e.name = name;
            e.factory = true;
            e.effectCount = (int)chain.size();
            entries_.push_back(e);
        }
        if (shown_ >= (int)entries_.size()) shown_ = entries_.empty() ? -1 : 0;
        if (shown_ < 0 && !entries_.empty()) shown_ = 0;
        updateName();
    }

    juce::String shownName() const { return shown_ >= 0 ? entries_[(size_t)shown_].name : juce::String(); }
    int activeSnapshot() const noexcept { return activeSnapshot_; }

    void paint(juce::Graphics& g) override
    {
        auto name = nameLabel_.getBounds().toFloat();
        g.setColour(KhensuTheme::bgRaised);
        g.fillRoundedRectangle(name, 4.0f);
        g.setColour(KhensuTheme::borderSubtle);
        g.drawRoundedRectangle(name.reduced(0.5f), 4.0f, 1.0f);
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
        g.drawText(TRANS("Rack"), rackLabel_, juce::Justification::centredRight, false);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        const int h = KhensuTheme::buttonHSmall;
        auto row = r.withSizeKeepingCentre(r.getWidth(), h);
        snapB_.setBounds(row.removeFromRight(32));
        row.removeFromRight(2);
        snapA_.setBounds(row.removeFromRight(32));
        row.removeFromRight(KhensuTheme::sp1);
        rackLabel_ = row.removeFromRight(36);
        row.removeFromRight(KhensuTheme::sp4);
        prevButton_.setBounds(row.removeFromLeft(h));
        row.removeFromLeft(2);
        menuButton_.setBounds(row.removeFromRight(h));
        row.removeFromRight(KhensuTheme::sp1);
        applyButton_.setBounds(row.removeFromRight(72));
        row.removeFromRight(KhensuTheme::sp1);
        nextButton_.setBounds(row.removeFromRight(h));
        row.removeFromRight(2);
        nameLabel_.setBounds(row);
    }

  private:
    void applyThemeColours()
    {
        nameLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    }

    void updateName()
    {
        if (shown_ < 0) {
            nameLabel_.setText(TRANS("No presets yet"), juce::dontSendNotification);
        } else {
            const auto& e = entries_[(size_t)shown_];
            nameLabel_.setText(e.factory ? e.name + " " + TRANS("(factory)") : e.name, juce::dontSendNotification);
            nameLabel_.setTooltip(preset_store::describe(e));
        }
        applyButton_.setEnabled(shown_ >= 0);
    }

    void step(int dir)
    {
        refresh();
        shown_ = preset_store::stepIndex(shown_, (int)entries_.size(), dir);
        updateName();
    }

    void applyShown()
    {
        if (shown_ < 0 || controller_.getActiveSession() == nullptr) return;
        const auto entry = entries_[(size_t)shown_];
        // Rack settings: staged in the one plugin chain; the rack applies them (WF-9).
        const auto params = preset_store::loadParams(entry);
        if (!params.empty()) controller_.lifecycle().stagePluginChain(params, TRANS("Preset %s").replace("%s", entry.name));
        IRenders::ChainRender render;
        render.chain = preset_store::load(entry);
        if (render.chain.empty()) return;
        render.description = "Preset \"" + entry.name + "\"";
        render.sourceRecommendationId = "preset_" + juce::File::createLegalFileName(entry.factory ? entry.name
                                                                                                  : entry.fileName);
        controller_.renders().renderVersion(render, "_preset_" + juce::File::createLegalFileName(entry.name),
                                            [](IRenders::RenderOutcome, const juce::String&, const juce::var&) {});
    }

    /// Saves the rack's current settings with the last applied chain as `name`.
    void saveAs(const juce::String& name)
    {
        juce::Component::SafePointer<PresetBar> self(this);
        controller_.lifecycle().fetchPluginParams([self, name](const std::map<juce::String, float>& params) {
            if (self == nullptr) return;
            if (auto saved = preset_store::save(name, self->controller_.renderJobs().lastChain(), params)) {
                self->refresh();
                for (size_t i = 0; i < self->entries_.size(); ++i)
                    if (!self->entries_[i].factory && self->entries_[i].fileName == saved->fileName)
                        self->shown_ = (int)i;
                self->updateName();
            }
        });
    }

    void showMenu()
    {
        refresh();
        juce::PopupMenu factoryMenu;
        int id = 100;
        for (const auto& [name, chain] : preset_store::factory()) {
            const juce::String n = name;
            factoryMenu.addItem(id++, n, true, false);
        }
        juce::PopupMenu menu;
        const bool userShown = shown_ >= 0 && !entries_[(size_t)shown_].factory;
        menu.addItem(1, TRANS("Save over shown preset"), userShown);
        menu.addItem(2, TRANS("Save rack and last chain as..."), true);
        menu.addItem(3, TRANS("Delete shown preset"), userShown);
        menu.addSeparator();
        menu.addSubMenu(TRANS("Factory presets"), factoryMenu);
        menu.addItem(5, TRANS("Open the presets folder"), true);
        juce::Component::SafePointer<PresetBar> self(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&menuButton_), [self](int result) {
            if (self != nullptr) self->menuChosen(result);
        });
    }

    void menuChosen(int result)
    {
        if (result == 1 && shown_ >= 0) {
            saveAs(entries_[(size_t)shown_].name);
        } else if (result == 5) {
            preset_store::directory().startAsProcess();
        } else if (result == 2) {
            askName();
        } else if (result == 3 && shown_ >= 0) {
            preset_store::remove(entries_[(size_t)shown_]);
            refresh();
        } else if (result >= 100) {
            const auto idx = (size_t)(result - 100);
            if (idx < preset_store::factory().size())
                for (size_t i = 0; i < entries_.size(); ++i)
                    if (entries_[i].factory && entries_[i].name == preset_store::factory()[idx].first) {
                        shown_ = (int)i;
                        updateName();
                    }
        }
    }

    void askName()
    {
        auto* window = new juce::AlertWindow(TRANS("Save preset as"),
                                             TRANS("Name the preset (the rack's settings and the last applied chain). "
                                                   "The Copilot plugin lists it too."),
                                             juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("name", {}, TRANS("Preset name"));
        window->addButton(TRANS("Save"), 1, juce::KeyPress(juce::KeyPress::returnKey));
        window->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<PresetBar> self(this);
        window->enterModalState(true, juce::ModalCallbackFunction::create([self, window](int r) {
            const auto name = window->getTextEditorContents("name").trim();
            if (self == nullptr || r != 1 || name.isEmpty()) return;
            self->saveAs(name);
        }), true);
    }

    void switchSnapshot(int slot)
    {
        if (slot == activeSnapshot_) return;
        // Keep what the rack holds now in the slot being left, then recall
        // the other slot (staged and applied through the one plugin chain, WF-9).
        juce::Component::SafePointer<PresetBar> self(this);
        controller_.lifecycle().fetchPluginParams([self, slot](const std::map<juce::String, float>& params) {
            if (self == nullptr) return;
            self->snapshots_[(size_t)self->activeSnapshot_] = params;
            self->activeSnapshot_ = slot;
            self->snapA_.setToggleState(slot == 0, juce::dontSendNotification);
            self->snapB_.setToggleState(slot == 1, juce::dontSendNotification);
            const auto& recall = self->snapshots_[(size_t)slot];
            if (!recall.empty()) {
                self->controller_.lifecycle().stagePluginChain(recall, slot == 0 ? TRANS("Rack snapshot A")
                                                                                   : TRANS("Rack snapshot B"));
                self->controller_.lifecycle().applyStagedPluginChain();
            }
        });
    }

    IAppController& controller_;
    std::vector<preset_store::Entry> entries_;
    int shown_ = -1;
    std::array<std::map<juce::String, float>, 2> snapshots_;
    int activeSnapshot_ = 0;
    juce::Rectangle<int> rackLabel_;

    juce::TextButton prevButton_{"<"}; // i18n-exempt(symbol; named by setTitle)
    juce::TextButton nextButton_{">"}; // i18n-exempt(symbol; named by setTitle)
    juce::TextButton menuButton_{"..."}; // i18n-exempt(symbol; named by setTitle)
    juce::TextButton applyButton_{TRANS("Apply")};
    juce::TextButton snapA_{"A"}; // i18n-exempt(snapshot slot; tooltip names it)
    juce::TextButton snapB_{"B"}; // i18n-exempt(snapshot slot; tooltip names it)
    juce::Label nameLabel_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetBar)
};

} // namespace khensu
