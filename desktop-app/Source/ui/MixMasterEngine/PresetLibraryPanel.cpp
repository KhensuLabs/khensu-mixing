// desktop-app/Source/ui/MixMasterEngine/PresetLibraryPanel.cpp
#include "PresetLibraryPanel.h"
#include "../../RenderJobManager.h"
#include <khensu/session/Session.h>
#include "../shared/KhensuTheme.h"
#include "../shared/RenderPaths.h"
#include <khensu/common/AppPaths.h>

namespace khensu {

struct PresetLibraryPanel::RowComponent final : public juce::Component {
    PresetRow row;
    std::function<void()> onApply, onDelete;

    juce::TextButton applyButton{TRANS("Apply")};
    juce::TextButton deleteButton{"x"}; // i18n-exempt(symbol; named by setTitle)

    RowComponent()
    {
        applyButton.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.18f));
        applyButton.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
        applyButton.onClick = [this] {
            if (onApply) onApply();
        };
        addAndMakeVisible(applyButton);

        deleteButton.setTitle(TRANS("Delete preset"));
        deleteButton.setTooltip(TRANS("Delete this preset (here and in the plugin's preset menu)"));
        deleteButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        deleteButton.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentRed);
        deleteButton.onClick = [this] {
            if (onDelete) onDelete();
        };
        addAndMakeVisible(deleteButton);
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().reduced(2);
        g.setColour(KhensuTheme::bgOverlay.withAlpha(0.5f));
        g.fillRoundedRectangle(b.toFloat(), 4.0f);

        auto inner = b.reduced(8, 4);
        inner.removeFromRight(applyButton.getWidth() + deleteButton.getWidth() + 12);
        g.setFont(KhensuTheme::bodyFont(type::caption).boldened());
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(row.name, inner.removeFromTop(inner.getHeight() / 2), juce::Justification::centredLeft, true);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(preset_store::describe(row) + (row.savedAt.isNotEmpty() ? "  -  " + row.savedAt.substring(0, 10) : juce::String()),
                   inner, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced(4);
        deleteButton.setBounds(b.removeFromRight(24));
        b.removeFromRight(4);
        applyButton.setBounds(b.removeFromRight(64));
    }
};

// Every cached palette colour on this component, in one place (UI-01).
void PresetLibraryPanel::applyThemeColours()
{
    saveButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    saveButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

PresetLibraryPanel::PresetLibraryPanel(IAppController& c) : PanelCard(c, "Preset Library")
{
    applyThemeColours();
    setExplainer(TRANS("Preset Library"),
                 TRANS("Save the Signal Flow Rack's settings and the most recently applied chain (from chat, Fix, "
                       "or an EQ apply) under a name, then re-apply it to this or a future session. Presets are "
                       "shared with the Copilot plugin: one saved here is in its preset menu, and one saved there "
                       "is listed here."));

    nameEditor_.setFont(KhensuTheme::bodyFont(type::caption));
    nameEditor_.setTextToShowWhenEmpty(TRANS("Preset name..."), KhensuTheme::textTertiary);
    addAndMakeVisible(nameEditor_);

    saveButton_.onClick = [this] { saveCurrentChain(); };
    addAndMakeVisible(saveButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(type::caption));
    statusLabel_.setText(TRANS("Saves the rack's settings and the last applied chain."), juce::dontSendNotification);
    addAndMakeVisible(statusLabel_);

    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);

    refreshList();
}

PresetLibraryPanel::~PresetLibraryPanel() = default;

void PresetLibraryPanel::visibilityChanged()
{
    // The plugin may have saved or deleted a preset meanwhile.
    if (isShowing()) refreshList();
}

void PresetLibraryPanel::saveCurrentChain()
{
    juce::String name = nameEditor_.getText().trim();
    if (name.isEmpty()) name = "Chain " + juce::Time::getCurrentTime().formatted("%Y-%m-%d %H%M%S");
    // The rack's settings (the plugin's DSP) with the last applied chain.
    juce::Component::SafePointer<PresetLibraryPanel> self(this);
    controller_.lifecycle().fetchPluginParams([self, name](const std::map<juce::String, float>& params) {
        if (self == nullptr) return;
        const auto& chain = self->controller_.renderJobs().lastChain();
        if (chain.empty() && params.empty()) {
            self->statusLabel_.setText(TRANS("Nothing to save yet - apply a chain (chat, Fix, EQ) or connect the "
                                             "plugin first."),
                                       juce::dontSendNotification);
            return;
        }
        if (preset_store::save(name, chain, params)) {
            self->statusLabel_.setText(TRANS("Saved preset: ") + name, juce::dontSendNotification);
            self->nameEditor_.clear();
            self->refreshList();
        } else {
            self->statusLabel_.setText(TRANS("Could not write preset file."), juce::dontSendNotification);
        }
    });
}

void PresetLibraryPanel::refreshList()
{
    presets_ = preset_store::list();

    rows_.clear();
    for (const auto& p : presets_) {
        auto rc = std::make_unique<RowComponent>();
        rc->row = p;
        rc->applyButton.setTitle(TRANS("Apply preset") + " " + p.name);
        rc->onApply = [this, p] { applyPreset(p); };
        rc->onDelete = [this, p] { deletePreset(p); };
        content_.addAndMakeVisible(*rc);
        rows_.push_back(std::move(rc));
    }
    resized();
    repaint();
}

void PresetLibraryPanel::applyPreset(const PresetRow& row)
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || s->files.empty()) {
        statusLabel_.setText(TRANS("No active session."), juce::dontSendNotification);
        return;
    }

    // Rack settings: staged in the one plugin chain; the rack applies them (WF-9).
    const auto params = preset_store::loadParams(row);
    if (!params.empty()) controller_.lifecycle().stagePluginChain(params, TRANS("Preset %s").replace("%s", row.name));

    IRenders::ChainRender render;
    render.chain = preset_store::load(row);
    if (render.chain.empty()) {
        statusLabel_.setText(params.empty() ? TRANS("Preset has nothing to apply: ") + row.name
                                            : TRANS("Rack settings staged - apply them on the Signal Flow Rack."),
                             juce::dontSendNotification);
        return;
    }

    statusLabel_.setText(TRANS("Applying preset: ") + row.name + "...", juce::dontSendNotification);

    // A new version (WF-1: stacked on the current one), named uniquely so a
    // fixed "_preset_<name>.wav" never overwrites a referenced render (AUDIT-018).
    render.description = "Preset \"" + row.name + "\"";
    render.sourceRecommendationId = "preset_" + juce::File::createLegalFileName(row.fileName);
    juce::Component::SafePointer<PresetLibraryPanel> self(this);
    controller_.renders().renderVersion(
        render, "_preset_" + juce::File::createLegalFileName(row.name),
        [self, name = row.name](IRenders::RenderOutcome outcome, const juce::String& pathOrError, const juce::var&) {
            if (self == nullptr) return;
            juce::String text;
            switch (outcome) {
            case IRenders::RenderOutcome::Rendered: text = TRANS("Applied preset: ") + name + TRANS(" - loaded as A/B."); break;
            case IRenders::RenderOutcome::Failed: text = TRANS("Apply failed: ") + pathOrError; break;
            case IRenders::RenderOutcome::Cancelled: text = TRANS("Preset render cancelled."); break;
            case IRenders::RenderOutcome::SessionChanged: text = TRANS("Session changed - the render was not loaded."); break;
            }
            self->statusLabel_.setText(text, juce::dontSendNotification);
        });
}

void PresetLibraryPanel::deletePreset(const PresetRow& row)
{
    if (preset_store::remove(row)) {
        statusLabel_.setText(TRANS("Deleted: ") + row.name, juce::dontSendNotification);
        refreshList();
    }
}

void PresetLibraryPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();

    auto controls = b.removeFromTop(24);
    saveButton_.setBounds(controls.removeFromRight(170));
    controls.removeFromRight(6);
    nameEditor_.setBounds(controls);
    b.removeFromTop(2);
    statusLabel_.setBounds(b.removeFromTop(14));
    b.removeFromTop(4);
    viewport_.setBounds(b);

    const int rowH = 40;
    content_.setSize(juce::jmax(50, viewport_.getWidth() - 10), juce::jmax(1, (int)rows_.size() * (rowH + 4)));
    int y = 0;
    for (auto& rc : rows_) {
        rc->setBounds(0, y, content_.getWidth(), rowH);
        y += rowH + 4;
    }
}

} // namespace khensu
