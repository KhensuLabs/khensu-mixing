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
    juce::TextButton deleteButton{TRANS("x")}; // x

    RowComponent()
    {
        applyButton.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.18f));
        applyButton.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
        applyButton.onClick = [this] {
            if (onApply) onApply();
        };
        addAndMakeVisible(applyButton);

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
        g.setFont(KhensuTheme::bodyFont(11.0f).boldened());
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(row.name, inner.removeFromTop(inner.getHeight() / 2), juce::Justification::centredLeft, true);
        g.setFont(KhensuTheme::labelFont(9.0f));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(juce::String(row.effectCount) + " effect(s)  -  " + row.savedAt, inner,
                   juce::Justification::centredLeft, true);
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
                 TRANS("Save the most recently applied DSP chain (from chat, \"Fix This\", or an EQ apply) "
                 "under a name, then re-apply it to this or a future session without rebuilding it. "
                 "Presets are stored as JSON on disk, so they survive across sessions and app restarts."));

    nameEditor_.setFont(KhensuTheme::bodyFont(11.0f));
    nameEditor_.setTextToShowWhenEmpty(TRANS("Preset name..."), KhensuTheme::textTertiary);
    addAndMakeVisible(nameEditor_);

    saveButton_.onClick = [this] { saveCurrentChain(); };
    addAndMakeVisible(saveButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(10.0f));
    statusLabel_.setText(TRANS("Apply a chain (chat, Fix This, EQ) first, then save it here."), juce::dontSendNotification);
    addAndMakeVisible(statusLabel_);

    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);

    refreshList();
}

PresetLibraryPanel::~PresetLibraryPanel() = default;

juce::File PresetLibraryPanel::presetDirectory()
{
    auto dir = juce::File(juce::String(AppPaths::dataDirectory())).getChildFile("effect_chain_presets");
    if (!dir.isDirectory()) dir.createDirectory();
    return dir;
}

void PresetLibraryPanel::saveCurrentChain()
{
    const auto& chain = controller_.renderJobs().lastChain();
    if (chain.empty()) {
        statusLabel_.setText(TRANS("No chain applied yet this session - apply one (chat, "
                             "Fix This, EQ) first."),
                             juce::dontSendNotification);
        return;
    }

    juce::String name = nameEditor_.getText().trim();
    if (name.isEmpty()) name = "Chain " + juce::Time::getCurrentTime().formatted("%Y-%m-%d %H%M%S");
    const juce::String legalName = juce::File::createLegalFileName(name);

    juce::Array<juce::var> effectsArr;
    for (const auto& fx : chain) {
        juce::DynamicObject::Ptr fxObj = new juce::DynamicObject();
        fxObj->setProperty("effect", fx.effect);
        juce::DynamicObject::Ptr paramsObj = new juce::DynamicObject();
        for (const auto& [key, value] : fx.parameters)
            paramsObj->setProperty(key, value);
        fxObj->setProperty("parameters", juce::var(paramsObj.get()));
        effectsArr.add(juce::var(fxObj.get()));
    }

    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("schema_version", 1);
    obj->setProperty("name", name);
    obj->setProperty("saved_at", juce::Time::getCurrentTime().toISO8601(true));
    obj->setProperty("effects", effectsArr);

    auto file = presetDirectory().getChildFile(legalName + ".json");
    if (file.replaceWithText(juce::JSON::toString(juce::var(obj.get()), false))) {
        statusLabel_.setText(TRANS("Saved preset: ") + name, juce::dontSendNotification);
        nameEditor_.clear();
        refreshList();
    } else {
        statusLabel_.setText(TRANS("Could not write preset file."), juce::dontSendNotification);
    }
}

void PresetLibraryPanel::refreshList()
{
    presets_.clear();
    for (const auto& file : presetDirectory().findChildFiles(juce::File::findFiles, false, "*.json")) {
        auto json = juce::JSON::parse(file.loadFileAsString());
        auto* obj = json.getDynamicObject();
        if (obj == nullptr) continue;

        PresetRow row;
        row.name = obj->getProperty("name").toString();
        row.fileName = file.getFileNameWithoutExtension();
        row.savedAt = obj->getProperty("saved_at").toString();
        if (auto* arr = obj->getProperty("effects").getArray()) row.effectCount = (int)arr->size();
        presets_.push_back(std::move(row));
    }
    std::sort(presets_.begin(), presets_.end(),
              [](const PresetRow& a, const PresetRow& b) { return a.savedAt > b.savedAt; });

    rows_.clear();
    for (const auto& p : presets_) {
        auto rc = std::make_unique<RowComponent>();
        rc->row = p;
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

    auto file = presetDirectory().getChildFile(row.fileName + ".json");
    auto json = juce::JSON::parse(file.loadFileAsString());
    auto* obj = json.getDynamicObject();
    if (obj == nullptr) {
        statusLabel_.setText(TRANS("Preset file is corrupt: ") + row.name, juce::dontSendNotification);
        return;
    }

    IRenders::ChainRender render;
    auto& chain = render.chain;
    if (auto* arr = obj->getProperty("effects").getArray()) {
        for (const auto& fxVar : *arr) {
            ipc::HttpServiceBridge::EffectSpec fx;
            fx.effect = fxVar["effect"].toString();
            if (auto* paramsObj = fxVar["parameters"].getDynamicObject())
                for (const auto& prop : paramsObj->getProperties())
                    fx.parameters[prop.name.toString()] = (float)(double)prop.value;
            chain.push_back(std::move(fx));
        }
    }
    if (chain.empty()) {
        statusLabel_.setText(TRANS("Preset has no effects: ") + row.name, juce::dontSendNotification);
        return;
    }

    statusLabel_.setText(TRANS("Applying preset: ") + row.name + "...", juce::dontSendNotification);

    // A new version (WF-1: stacked on the current one), named uniquely so a
    // fixed "_preset_<name>.wav" never overwrites a referenced render (AUDIT-018).
    render.description = "Preset \"" + row.name + "\"";
    render.sourceRecommendationId = "preset_" + row.fileName;
    juce::Component::SafePointer<PresetLibraryPanel> self(this);
    controller_.renders().renderVersion(
        render, "_preset_" + row.fileName,
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
    auto file = presetDirectory().getChildFile(row.fileName + ".json");
    if (file.deleteFile()) {
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
