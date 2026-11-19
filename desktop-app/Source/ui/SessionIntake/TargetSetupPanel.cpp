// desktop-app/Source/ui/SessionIntake/TargetSetupPanel.cpp
#include "TargetSetupPanel.h"
#include "../shared/KhensuLookAndFeel.h"
#include <khensu/targets/GenreDatabase.h>
#include <khensu/targets/GenreEntry.h>
#include <algorithm>

namespace khensu {

const char* TargetSetupPanel::kSubGenreNoneLabel = "(none / unknown)";

void TargetSetupPanel::configurePct(juce::Slider& s, double initial)
{
    s.setRange(0.0, 100.0, 1.0);
    s.setValue(initial, juce::dontSendNotification);
    s.setColour(juce::Slider::trackColourId, KhensuTheme::accentTeal);
    addAndMakeVisible(s);
}

void TargetSetupPanel::configureRowLabel(juce::Label& l, const juce::String& text)
{
    l.setText(text, juce::dontSendNotification);
    l.setFont(khensu::KhensuTheme::uiFont(13.0f, juce::Font::plain));
    l.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    addAndMakeVisible(l);
}

const std::vector<TargetSetupPanel::QuickStartTemplate>& TargetSetupPanel::quickStartTemplates()
{
    static const std::vector<QuickStartTemplate> templates = {
        {"Pop Vocal", "pop", "", "uplifting", 65.0f, 118.0f, 70.0f, 65.0f, 20.0f, 65.0f},
        {"Hip-Hop / Trap", "rap", "trap", "confident", 70.0f, 140.0f, 65.0f, 55.0f, 45.0f, 60.0f},
        {"EDM / House Drop", "house", "", "euphoric", 85.0f, 126.0f, 75.0f, 80.0f, 55.0f, 70.0f},
        {"Rock / Alt", "alt-rock", "", "driving", 75.0f, 130.0f, 55.0f, 50.0f, 70.0f, 55.0f},
        {"R&B / Neo-Soul", "neo-soul", "", "sultry", 45.0f, 90.0f, 50.0f, 55.0f, 15.0f, 60.0f},
        {"Lo-fi / Chill", "", "lo-fi", "melancholic", 30.0f, 85.0f, 35.0f, 40.0f, 10.0f, 45.0f},
        {"Acoustic / Ballad", "", "acoustic", "intimate", 25.0f, 75.0f, 40.0f, 60.0f, 5.0f, 55.0f},
    };
    return templates;
}

void TargetSetupPanel::applyQuickStartTemplate(int index)
{
    const auto& templates = quickStartTemplates();
    if (index < 0 || index >= (int)templates.size()) return;
    const auto& t = templates[(size_t)index];
    autoPopulateFromAnalysis(t.genre, t.subgenre, t.mood, t.energy, t.bpm, /*key*/ {});
    setIntentTargets(t.hook, t.payoff, t.heaviness, t.memorability);
    genreStatedByUser_ = true; // a template is the user's choice
}

// Every cached palette colour on this component, in one place (UI-01).
void TargetSetupPanel::applyThemeColours()
{
    headerLabel.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    keyValueLabel.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    bpmSlider.setColour(juce::Slider::trackColourId, KhensuTheme::accentTeal);
    deepAnalysisToggle.setColour(juce::ToggleButton::tickColourId, KhensuTheme::accentTeal);
    deepAnalysisToggle.setColour(juce::ToggleButton::textColourId, KhensuTheme::textPrimary);
}

TargetSetupPanel::TargetSetupPanel(const GenreDatabase& genres)
{
    applyThemeColours();
    configureRowLabel(quickStartLabel, "Quick start");
    quickStartCombo.addItem(TRANS("(choose a starting point...)"), 1);
    int qsId = 2;
    for (const auto& t : quickStartTemplates())
        quickStartCombo.addItem(t.label, qsId++);
    quickStartCombo.setSelectedId(1, juce::dontSendNotification);
    quickStartCombo.onChange = [this] {
        const int sel = quickStartCombo.getSelectedId() - 2; // 0-based into templates
        if (sel >= 0) applyQuickStartTemplate(sel);
    };
    addAndMakeVisible(quickStartCombo);

    headerLabel.setText(TRANS("TARGET SETUP"), juce::dontSendNotification);
    headerLabel.setFont(khensu::KhensuTheme::uiFont(16.0f, juce::Font::bold));
    addAndMakeVisible(headerLabel);

    // Genre combo from DB (item ids are 1-based; index 0 reserved for "unset").
    configureRowLabel(genreLabel, "Genre");
    genreCombo.addItem(TRANS("(unspecified)"), 1);
    genreIds.push_back("");
    int itemId = 2;
    for (const auto& entry : genres.listAll()) {
        const juce::String name = juce::String(entry.name.empty() ? entry.id : entry.name);
        genreCombo.addItem(name, itemId++);
        genreIds.push_back(juce::String(entry.id));

        // Build the sub-genre grouping: family label -> member names, and each
        // genre id -> its family, so picking a genre can narrow the sub-genre list.
        const std::string family = entry.parent.empty() ? entry.id : entry.parent;
        genreIdToFamily_[entry.id] = family;
        familyToSubs_[family].push_back(name);
        allSubNames_.push_back(name);
    }
    for (auto& [fam, names] : familyToSubs_)
        std::sort(names.begin(), names.end());
    std::sort(allSubNames_.begin(), allSubNames_.end());
    genreCombo.setSelectedId(1, juce::dontSendNotification);
    // Narrow the sub-genre list to the chosen genre's family when the genre changes.
    genreCombo.onChange = [this] {
        if (!fillingFromAnalysis_) genreStatedByUser_ = true;
        const int gi = genreCombo.getSelectedId() - 1;
        populateSubGenreForGenreId((gi >= 0 && gi < (int)genreIds.size()) ? genreIds[(size_t)gi] : juce::String());
    };
    addAndMakeVisible(genreCombo);

    // Sub-genre: an editable dropdown (pick a family member OR type your own),
    // with "(none / unknown)" as the default. Populated for the current genre.
    configureRowLabel(subGenreLabel, "Sub-genre");
    subGenreCombo.setEditableText(true);
    subGenreCombo.setTextWhenNothingSelected(TRANS("e.g. melodic trap"));
    populateSubGenreForGenreId({});
    addAndMakeVisible(subGenreCombo);

    configureRowLabel(moodLabel, "Mood");
    moodEditor.setTextToShowWhenEmpty(TRANS("e.g. melancholic"), KhensuTheme::textSecondary);
    addAndMakeVisible(moodEditor);

    configureRowLabel(keyLabel, "Key");
    keyValueLabel.setText(TRANS("(auto-detected on load)"), juce::dontSendNotification);
    keyValueLabel.setFont(khensu::KhensuTheme::uiFont(13.0f, juce::Font::plain));
    addAndMakeVisible(keyValueLabel);

    configureRowLabel(energyLabel, "Energy");
    configurePct(energySlider, 50.0);

    configureRowLabel(bpmLabel, "BPM");
    bpmSlider.setRange(0.0, 220.0, 1.0);
    bpmSlider.setValue(0.0, juce::dontSendNotification);
    addAndMakeVisible(bpmSlider);

    configureRowLabel(intentLabel, "Playback");
    intentCombo.addItem(TRANS("streaming"), 1);
    intentCombo.addItem(TRANS("club"), 2);
    intentCombo.addItem(TRANS("broadcast"), 3);
    intentCombo.addItem(TRANS("headphone"), 4);
    intentCombo.addItem(TRANS("reference"), 5);
    intentCombo.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(intentCombo);

    configureRowLabel(hookLabel, "Hook emphasis");
    configurePct(hookSlider, 50.0);
    configureRowLabel(payoffLabel, "Chorus payoff");
    configurePct(payoffSlider, 50.0);
    configureRowLabel(heavyLabel, "Heaviness");
    configurePct(heavySlider, 50.0);
    configureRowLabel(memoLabel, "Memorability");
    configurePct(memoSlider, 50.0);

    deepAnalysisToggle.setToggleState(true, juce::dontSendNotification);
    addAndMakeVisible(deepAnalysisToggle);

    configureRowLabel(visionLabel, "Vision");
    visionEditor.setMultiLine(true);
    visionEditor.setReturnKeyStartsNewLine(true);
    visionEditor.setTextToShowWhenEmpty(TRANS("e.g. a bedroom at 3am in summer"), KhensuTheme::textSecondary);
    addAndMakeVisible(visionEditor);
}

void TargetSetupPanel::populateSubGenreForGenreId(const juce::String& genreId)
{
    // Preserve any custom text the user already typed that isn't a list item, so
    // switching genres doesn't silently wipe a hand-entered sub-genre.
    const juce::String existing = subGenreCombo.getText();
    const bool existingIsCustom =
        existing.isNotEmpty() && existing != kSubGenreNoneLabel && subGenreCombo.getSelectedId() == 0;

    subGenreCombo.clear(juce::dontSendNotification);
    subGenreCombo.addItem(kSubGenreNoneLabel, 1);

    const std::vector<juce::String>* names = &allSubNames_;
    if (genreId.isNotEmpty()) {
        const auto fam = genreIdToFamily_.find(genreId.toStdString());
        if (fam != genreIdToFamily_.end()) {
            const auto it = familyToSubs_.find(fam->second);
            if (it != familyToSubs_.end()) names = &it->second;
        }
    }
    int id = 2;
    for (const auto& n : *names)
        subGenreCombo.addItem(n, id++);

    if (existingIsCustom)
        subGenreCombo.setText(existing, juce::dontSendNotification); // keep the user's typed value
    else
        subGenreCombo.setSelectedId(1, juce::dontSendNotification); // default to none/unknown
}

CreativeTarget TargetSetupPanel::buildTarget() const
{
    CreativeTarget t;
    const int gi = genreCombo.getSelectedId() - 1; // 0-based into genreIds
    if (gi >= 0 && gi < (int)genreIds.size()) t.primary_genre = genreIds[(size_t)gi].toStdString();

    // "(none / unknown)" is a sentinel, not a real sub-genre -> store empty.
    const juce::String sub = subGenreCombo.getText().trim();
    t.sub_genre = (sub == kSubGenreNoneLabel) ? std::string{} : sub.toStdString();
    t.mood = moodEditor.getText().toStdString();
    t.energy = (float)energySlider.getValue();
    t.bpm = (float)bpmSlider.getValue();
    t.playback_intent = playbackIntentIdForComboId(intentCombo.getSelectedId());
    t.hook_emphasis = (float)hookSlider.getValue();
    t.chorus_payoff = (float)payoffSlider.getValue();
    t.heaviness_target = (float)heavySlider.getValue();
    t.memorability_target = (float)memoSlider.getValue();
    t.vision_statement = visionEditor.getText().toStdString();
    t.run_deep_analysis = deepAnalysisToggle.getToggleState();
    return t;
}

std::string TargetSetupPanel::playbackIntentIdForComboId(int comboId)
{
    // Same order as the combo items in the constructor.
    static const char* const kIds[] = {"streaming", "club", "broadcast", "headphone", "reference"};
    return comboId >= 1 && comboId <= (int)std::size(kIds) ? std::string(kIds[comboId - 1]) : std::string{};
}

void TargetSetupPanel::autoSelectGenre(const juce::String& genre, const juce::String& subgenre)
{
    // The Python detector emits coarse labels (pop/hiphop/rnb/rock/electronic)
    // while the genre DB uses specific ids (trap, alt-rock, house, ...). Map
    // the coarse labels to representative DB entries - without this, 4 of the
    // detector's 5 outputs silently selected nothing.
    static const std::pair<const char*, const char*> kDetectorAliases[] = {
        {"hiphop", "rap"},    {"hip-hop", "rap"},      {"rnb", "neo-soul"}, {"r&b", "neo-soul"},
        {"rock", "alt-rock"}, {"electronic", "house"}, {"edm", "house"},
    };

    juce::String lookup = genre;
    for (const auto& [from, to] : kDetectorAliases)
        if (lookup.equalsIgnoreCase(from)) {
            lookup = to;
            break;
        }

    auto selectById = [this](const juce::String& id) -> bool {
        for (int i = 0; i < (int)genreIds.size(); ++i) {
            if (genreIds[(size_t)i].equalsIgnoreCase(id)) {
                genreCombo.setSelectedId(i + 1, juce::sendNotificationSync);
                return true;
            }
        }
        return false;
    };

    if (!selectById(lookup) && lookup.isNotEmpty()) {
        // Last resort: substring match against DB ids (e.g. a future detector
        // label "uk-drill" -> "drill").
        for (int i = 0; i < (int)genreIds.size(); ++i) {
            if (genreIds[(size_t)i].containsIgnoreCase(lookup) || lookup.containsIgnoreCase(genreIds[(size_t)i])) {
                genreCombo.setSelectedId(i + 1, juce::sendNotificationSync);
                break;
            }
        }
    }
    // Synchronous on purpose: genreCombo.onChange must run now - it repopulates
    // the sub-genre list (an async change wiped the sub-genre set below) and
    // tells a user's pick from an analysis fill (genreStatedByUser, UX-12).
    // selectById above fired genreCombo.onChange, which repopulated the sub-genre
    // list for the new family and reset it to none. Apply the detected sub-genre
    // (or leave the none/unknown default when there isn't one).
    if (subgenre.isNotEmpty())
        subGenreCombo.setText(subgenre, juce::dontSendNotification);
    else
        subGenreCombo.setSelectedId(1, juce::dontSendNotification);
}

void TargetSetupPanel::autoPopulateFromAnalysis(const juce::String& genre, const juce::String& subgenre,
                                                const juce::String& mood, float energy0to100, float bpm,
                                                const juce::String& key)
{
    fillingFromAnalysis_ = true;
    autoSelectGenre(genre, subgenre);
    fillingFromAnalysis_ = false;

    if (mood.isNotEmpty()) moodEditor.setText(mood, juce::sendNotification);

    if (energy0to100 > 0.0f)
        energySlider.setValue(juce::jlimit(0.0, 100.0, (double)energy0to100), juce::sendNotification);

    if (bpm > 0.0f) bpmSlider.setValue(juce::jlimit(0.0, 220.0, (double)bpm), juce::sendNotification);

    if (key.isNotEmpty()) keyValueLabel.setText(key, juce::dontSendNotification);
}

void TargetSetupPanel::setIntentTargets(float hookEmphasis, float chorusPayoff, float heaviness, float memorability)
{
    if (hookEmphasis > 0.0f)
        hookSlider.setValue(juce::jlimit(0.0, 100.0, (double)hookEmphasis), juce::sendNotification);

    if (chorusPayoff > 0.0f)
        payoffSlider.setValue(juce::jlimit(0.0, 100.0, (double)chorusPayoff), juce::sendNotification);

    if (heaviness > 0.0f) heavySlider.setValue(juce::jlimit(0.0, 100.0, (double)heaviness), juce::sendNotification);

    if (memorability > 0.0f)
        memoSlider.setValue(juce::jlimit(0.0, 100.0, (double)memorability), juce::sendNotification);
}

void TargetSetupPanel::paint(juce::Graphics& g)
{
    g.setColour(KhensuTheme::bgSurface.withAlpha(0.4f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
}

void TargetSetupPanel::resized()
{
    auto b = getLocalBounds().reduced(16);
    headerLabel.setBounds(b.removeFromTop(24));
    b.removeFromTop(8);

    // Responsive fill: the 12 label+field rows share the height left after the
    // toggle + vision block, so the whole form fits its bounds statically - no
    // scrolling, no clipping - and grows to fill a tall column.
    constexpr int kRows = 12, kGap = 6;
    constexpr int kToggleH = 24, kVisionLabelH = 20, kVisionMinH = 44;
    const int reserved = kToggleH + kGap + kVisionLabelH + kVisionMinH;
    const int rowsArea = b.getHeight() - reserved;
    const int rowH = juce::jlimit(20, 40, (rowsArea - kRows * kGap) / kRows);

    auto row = [&b](juce::Component& label, juce::Component& field, int h) {
        auto r = b.removeFromTop(h);
        label.setBounds(r.removeFromLeft(110));
        field.setBounds(r);
        b.removeFromTop(kGap);
    };

    row(quickStartLabel, quickStartCombo, rowH);
    row(genreLabel, genreCombo, rowH);
    row(subGenreLabel, subGenreCombo, rowH);
    row(moodLabel, moodEditor, rowH);
    row(keyLabel, keyValueLabel, rowH);
    row(energyLabel, energySlider, rowH);
    row(bpmLabel, bpmSlider, rowH);
    row(intentLabel, intentCombo, rowH);
    row(hookLabel, hookSlider, rowH);
    row(payoffLabel, payoffSlider, rowH);
    row(heavyLabel, heavySlider, rowH);
    row(memoLabel, memoSlider, rowH);

    auto toggleRow = b.removeFromTop(kToggleH);
    deepAnalysisToggle.setBounds(toggleRow.removeFromLeft(250));
    b.removeFromTop(kGap);

    visionLabel.setBounds(b.removeFromTop(kVisionLabelH));
    visionEditor.setBounds(b); // fills whatever remains
}

} // namespace khensu
