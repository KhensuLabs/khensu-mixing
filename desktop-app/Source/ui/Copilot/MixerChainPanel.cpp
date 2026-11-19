// desktop-app/Source/ui/Copilot/MixerChainPanel.cpp
#include <algorithm>
#include "MixerChainPanel.h"
#include "../shared/JobPoller.h"
#include <khensu/ipc/generated/ApiJobStatus.h>
#include "SessionChainContext.h"
#include "../shared/KhensuTheme.h"
#include "../../IAppController.h"
#include <khensu/common/AppPaths.h>
#include <khensu/mixerchain/DspChainMapper.h>
#include <khensu/mixerchain/PluginScanner.h>
#include <khensu/targets/UserPreferenceModel.h>
#include <map>
#include <thread>

namespace khensu {

namespace {
constexpr int kCardH = 126;
constexpr int kCardGap = 8;

// Maps a generated card's free-form category/name text onto the generic
// effect vocabulary matcher.py's EFFECT_MAP_RULES understands. Returns an
// empty string when no rule fits (the slot is skipped, not force-matched).
// Order matters: specific types (de-esser, limiter) before their broader
// families (compressor); "eq" is whole-word-only so "frequency" won't match.
juce::String genericEffectForCategory(const juce::String& category, const juce::String& name)
{
    const auto s = (category + " " + name).toLowerCase();
    if (s.contains("de-ess") || s.contains("deess") || s.contains("sibil")) return "DeEsser";
    if (s.contains("limit") || s.contains("maximiz")) return "Limiter";
    if (s.contains("sidechain")) return "SidechainCompressor";
    if (s.contains("compress") || s.contains("glue") || s.contains("dynamic")) return "Compressor";
    if (s.contains("gate") || s.contains("expander")) return "NoiseGate";
    if (s.contains("reverb") || s.contains("room") || s.contains("hall") || s.contains("plate")) return "Reverb";
    if (s.contains("delay") || s.contains("echo")) return "Delay";
    if (s.contains("satur") || s.contains("distort") || s.contains("drive") || s.contains("tape") ||
        s.contains("clip") || s.contains("excit"))
        return "Distortion";
    if (s.contains("chorus")) return "Chorus";
    if (s.contains("phaser")) return "Phaser";
    if (s.contains("highpass") || s.contains("high-pass") || s.contains("hpf")) return "HighpassFilter";
    if (s.containsWholeWord("eq") || s.contains("equal") || s.contains("filter")) return "PeakFilter";
    return {};
}

void selectComboText(juce::ComboBox& box, const juce::String& text)
{
    for (int i = 0; i < box.getNumItems(); ++i) {
        if (box.getItemText(i).equalsIgnoreCase(text)) {
            box.setSelectedItemIndex(i, juce::dontSendNotification);
            return;
        }
    }
}
} // anonymous namespace

// ────────────────────────────────────────────────────────────────────────────
// ConsensusChartComponent
// ────────────────────────────────────────────────────────────────────────────

void MixerChainPanel::ConsensusChartComponent::paint(juce::Graphics& g)
{
    if (!info.has_value()) return;
    const auto& c = *info;
    auto b = getLocalBounds().toFloat();

    g.setColour(KhensuTheme::textSecondary);
    g.setFont(juce::Font(juce::FontOptions{8.f}));
    g.drawText("PROVIDER CONSENSUS", b.removeFromTop(11.f), juce::Justification::centredLeft);

    const int n = c.providers_queried.size();
    if (n > 0) {
        auto row = b.removeFromTop(14.f);
        const float bw = row.getWidth() / (float)n;
        for (int i = 0; i < n; ++i) {
            const bool responded = i < c.providers_responded;
            auto cell = row.removeFromLeft(bw).reduced(1.5f, 0.0f);
            g.setColour((responded ? KhensuTheme::accentTeal : KhensuTheme::borderSubtle).withAlpha(responded ? 0.8f : 0.5f));
            g.fillRoundedRectangle(cell, 2.0f);
            g.setColour(KhensuTheme::bgVoid.withAlpha(0.8f));
            g.setFont(juce::Font(juce::FontOptions{7.f}));
            g.drawText(c.providers_queried[i], cell.toNearestInt(), juce::Justification::centred, false);
        }
    }

    if (c.agreement_score.has_value()) {
        b.removeFromTop(3.0f);
        auto scoreRow = b.removeFromTop(10.f);
        const float frac = juce::jlimit(0.0f, 1.0f, (float)*c.agreement_score);
        g.setColour(KhensuTheme::borderSubtle);
        g.fillRect(scoreRow);
        g.setColour(KhensuTheme::accentBlue);
        g.fillRect(scoreRow.withWidth(scoreRow.getWidth() * frac));
        g.setColour(KhensuTheme::textPrimary.withAlpha(0.85f));
        g.setFont(juce::Font(juce::FontOptions{7.5f}));
        g.drawText(juce::String(juce::roundToInt(frac * 100.0f)) + "% agreement", scoreRow, juce::Justification::centred, false);
    }
}

// ────────────────────────────────────────────────────────────────────────────
// PluginCard
// ────────────────────────────────────────────────────────────────────────────

void MixerChainPanel::PluginCard::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced(2.f);
    g.setColour(KhensuTheme::bgRaised);
    g.fillRoundedRectangle(b, 8.f);
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.35f));
    g.drawRoundedRectangle(b, 8.f, 1.f);
}

void MixerChainPanel::PluginCard::applyColours()
{
    nameLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
    categoryLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    ecosystemLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentBlue);
    settingsLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    matchLabel_.setColour(juce::Label::textColourId,
                          matched_ ? KhensuTheme::accentTeal : KhensuTheme::textSecondary.withAlpha(0.75f));
}

void MixerChainPanel::PluginCard::resized()
{
    auto b = getLocalBounds().reduced(8);
    nameLabel_.setBounds(b.removeFromTop(22));
    categoryLabel_.setBounds(b.removeFromTop(18));
    ecosystemLabel_.setBounds(b.removeFromTop(18));
    matchLabel_.setBounds(b.removeFromTop(16));
    settingsLabel_.setBounds(b);
}

// ────────────────────────────────────────────────────────────────────────────
// MixerChainPanel
// ────────────────────────────────────────────────────────────────────────────

// Every cached palette colour on this panel, in one place (UI-01).
void MixerChainPanel::applyThemeColours()
{
    for (auto* l : {&targetLabel_, &genreLabel_, &energyLabel_, &moodLabel_, &bpmLabel_, &styleLabel_})
        l->setColour(juce::Label::textColourId, KhensuTheme::textSecondary);

    for (auto* e : {&bpmField_, &styleField_, &presetNameField_}) {
        e->setColour(juce::TextEditor::backgroundColourId, KhensuTheme::bgRaised);
        e->setColour(juce::TextEditor::outlineColourId, KhensuTheme::borderSubtle);
        e->setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
    }
    bpmField_.setTextToShowWhenEmpty(TRANS("BPM (e.g. 120)"), KhensuTheme::textSecondary);
    styleField_.setTextToShowWhenEmpty(TRANS("Describe the style, vibe, or reference..."), KhensuTheme::textSecondary);

    generateButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal);
    generateButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::bgVoid);
    useSessionTargetButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentBlue.withAlpha(0.2f));
    useSessionTargetButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentBlue);
    cancelGenerateButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentRed.darker(0.6f));
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    ecosystemCoverageBadge_.setColour(juce::Label::backgroundColourId, KhensuTheme::accentTeal.withAlpha(0.15f));
    ecosystemCoverageBadge_.setColour(juce::Label::outlineColourId, KhensuTheme::accentTeal.withAlpha(0.4f));
    ecosystemCoverageBadge_.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
    savePresetButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentBlue.withAlpha(0.25f));
    savePresetButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentBlue);
    pushToPluginDspButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    pushToPluginDspButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    mappingReportButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentBlue.withAlpha(0.2f));
    mappingReportButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentBlue);
    decisionTraceButton_.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    decisionTraceButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
    scanPluginsButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    scanPluginsButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    retrySkippedButton_.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    retrySkippedButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
    libraryBadge_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

MixerChainPanel::MixerChainPanel(IAppController& c) : controller_(c)
{
    applyThemeColours();
    auto setupLabel = [&](juce::Label& l) {
        l.setFont(juce::Font(juce::FontOptions{11.f}));
        addAndMakeVisible(l);
    };
    setupLabel(targetLabel_);
    setupLabel(genreLabel_);
    setupLabel(energyLabel_);
    setupLabel(moodLabel_);
    setupLabel(bpmLabel_);
    setupLabel(styleLabel_);

    // Target channel
    targetBox_.addItem("Vocals", 1);
    targetBox_.addItem("Instruments", 2);
    targetBox_.addItem("Mixer Bus", 3);
    targetBox_.addItem("Master", 4);
    targetBox_.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(targetBox_);

    // Genre
    for (const char* g :
         {"Pop", "Rock", "Hip-Hop", "Electronic", "Jazz", "Acoustic", "R&B", "Metal", "Classical", "Ambient"})
        genreBox_.addItem(g, genreBox_.getNumItems() + 1);
    genreBox_.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(genreBox_);

    // Energy
    energyBox_.addItem("Low", 1);
    energyBox_.addItem("Mid", 2);
    energyBox_.addItem("High", 3);
    energyBox_.setSelectedId(3, juce::dontSendNotification);
    addAndMakeVisible(energyBox_);

    // Mood
    for (const char* m :
         {"Modern", "Dreamy", "Aggressive", "Warm", "Bright", "Vintage", "Clean", "Nostalgic", "Dark", "Uplifting"})
        moodBox_.addItem(m, moodBox_.getNumItems() + 1);
    moodBox_.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(moodBox_);

    // BPM
    bpmField_.setInputRestrictions(6, "0123456789."); // allow decimal BPM (e.g. 89.5)
    addAndMakeVisible(bpmField_);

    // Style description
    styleField_.setMultiLine(true);
    styleField_.setReturnKeyStartsNewLine(false);
    addAndMakeVisible(styleField_);

    useSessionTargetButton_.setTooltip(TRANS("Fill genre, energy, mood, and BPM from the active session template."));
    useSessionTargetButton_.onClick = [this] { applySessionTarget(); };
    addAndMakeVisible(useSessionTargetButton_);

    // Generate button
    generateButton_.onClick = [this] { generate(); };
    addAndMakeVisible(generateButton_);

    cancelGenerateButton_.onClick = [this] { cancelGeneration(); };
    // addAndMakeVisible() unconditionally forces visible=true, so the initial
    // "hidden until a generation is in flight" state has to be set after it,
    // not before.
    addAndMakeVisible(cancelGenerateButton_);
    cancelGenerateButton_.setVisible(false);

    // Ecosystem toggles - all on by default
    for (int i = 0; i < kNumEco; ++i) {
        ecosystemToggles_[i].setButtonText(kEcoNames[i]);
        ecosystemToggles_[i].setToggleState(true, juce::dontSendNotification);
        ecosystemToggles_[i].setColour(juce::ToggleButton::textColourId, KhensuTheme::textPrimary);
        ecosystemToggles_[i].setColour(juce::ToggleButton::tickColourId, KhensuTheme::accentTeal);
        ecosystemToggles_[i].setColour(juce::ToggleButton::tickDisabledColourId, KhensuTheme::textSecondary);
        addAndMakeVisible(ecosystemToggles_[i]);
    }

    // Status
    statusLabel_.setFont(juce::Font(juce::FontOptions{12.f}));
    statusLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(statusLabel_);

    // Ecosystem Coverage Badge
    ecosystemCoverageBadge_.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::bold));
    ecosystemCoverageBadge_.setJustificationType(juce::Justification::centred);
    ecosystemCoverageBadge_.setVisible(false);
    addChildComponent(ecosystemCoverageBadge_);

    // Preset save / load
    presetNameField_.setTextToShowWhenEmpty(TRANS("Preset name..."), KhensuTheme::textSecondary);
    addAndMakeVisible(presetNameField_);

    savePresetButton_.setEnabled(false); // enabled once a chain exists
    savePresetButton_.onClick = [this] { savePreset(); };
    addAndMakeVisible(savePresetButton_);

    pushToPluginDspButton_.setEnabled(false); // enabled once a chain exists
    pushToPluginDspButton_.onClick = [this] { pushToPluginDsp(); };
    addAndMakeVisible(pushToPluginDspButton_);
    pluginTarget_ = std::make_unique<PluginTargetChooser>(
        [this] { return controller_.lifecycle().knownPluginInstances(); }, [this] { return controller_.lifecycle().pluginTargetInstance(); },
        [this](const juce::String& id) { controller_.lifecycle().setPluginTargetInstance(id); });
    addAndMakeVisible(*pluginTarget_);

    mappingReportButton_.setEnabled(false);
    mappingReportButton_.onClick = [this] { showMappingReport(); };
    addAndMakeVisible(mappingReportButton_);

    decisionTraceButton_.setEnabled(false);
    decisionTraceButton_.setTooltip(TRANS("Show the decision_hash fingerprint of whatever produced this chain."));
    decisionTraceButton_.onClick = [this] { showDecisionTrace(); };
    addAndMakeVisible(decisionTraceButton_);

    consensusBadge_.setFont(juce::Font(juce::FontOptions{10.f, juce::Font::bold}));
    consensusBadge_.setJustificationType(juce::Justification::centredLeft);
    consensusBadge_.setVisible(false);
    addChildComponent(consensusBadge_);
    consensusChart_.setVisible(false);
    addChildComponent(consensusChart_);

    scanPluginsButton_.onClick = [this] { scanPlugins(); };
    addAndMakeVisible(scanPluginsButton_);
    retrySkippedButton_.setTooltip(TRANS("Probe the plugins a scan skipped (they crashed or stopped responding) "
                                         "again, one at a time, with time to answer a licence or sign-in window."));
    retrySkippedButton_.onClick = [this] { retrySkippedPlugins(); };
    addChildComponent(retrySkippedButton_);

    libraryBadge_.setFont(juce::Font(juce::FontOptions{10.f}));
    libraryBadge_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(libraryBadge_);
    refreshLibraryBadge();

    presetBox_.setTextWhenNothingSelected(TRANS("Load preset..."));
    presetBox_.onChange = [this] {
        const int idx = presetBox_.getSelectedItemIndex();
        if (idx < 0) return;
        auto file = presetDirectory().getChildFile(presetBox_.getItemText(idx) + ".json");
        if (file.existsAsFile()) loadPreset(file);
    };
    addAndMakeVisible(presetBox_);
    refreshPresetList();

    // Results viewport
    resultsViewport_.setViewedComponent(&resultsGrid_, false);
    resultsViewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(resultsViewport_);
}

MixerChainPanel::~MixerChainPanel()
{
    if (pollStop_) pollStop_->store(true); // the poller cancels the job server-side
}

void MixerChainPanel::paint(juce::Graphics& g)
{
    g.fillAll(KhensuTheme::bgBase);

    // Header bar
    auto header = getLocalBounds().removeFromTop(40).toFloat();
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.12f));
    g.fillRect(header);
    g.setColour(KhensuTheme::accentTeal);
    g.setFont(juce::Font(juce::FontOptions{15.f, juce::Font::bold}));
    g.drawText("  " + TRANS("Mixer Chain Generator"), header.toNearestInt(), juce::Justification::centredLeft);

    // Column separators
    int w = getWidth();
    int leftW = w * 30 / 100;
    int midW = w * 25 / 100;
    g.setColour(KhensuTheme::borderSubtle);
    g.fillRect(leftW, 40, 1, getHeight() - 40);
    g.fillRect(leftW + midW, 40, 1, getHeight() - 40);
}

void MixerChainPanel::resized()
{
    const int w = getWidth();
    const int h = getHeight();
    const int top = 40; // header
    const int leftW = w * 30 / 100;
    const int midW = w * 25 / 100;
    const int pad = 8;

    // ── Left column: context controls
    // Clamp before reducing. A panel laid out shorter than its own header makes
    // (h - top) negative, and Rectangle::removeFromTop() then does
    // `jmin(amount, negative)` - handing a negative height to every widget in
    // the column below (kimi audit P1 #28). removeFromTop() clamps correctly
    // for any NON-negative height, so guarding the seed rectangle is enough.
    auto left = juce::Rectangle<int>(0, top, juce::jmax(0, leftW), juce::jmax(0, h - top)).reduced(pad);
    if (left.getWidth() < 0 || left.getHeight() < 0) left = {left.getX(), left.getY(), 0, 0};
    auto row = [&](int rowH) -> juce::Rectangle<int> { return left.removeFromTop(rowH); };
    targetLabel_.setBounds(row(14));
    targetBox_.setBounds(row(24));
    left.removeFromTop(4);
    genreLabel_.setBounds(row(14));
    genreBox_.setBounds(row(24));
    left.removeFromTop(4);
    energyLabel_.setBounds(row(14));
    energyBox_.setBounds(row(24));
    left.removeFromTop(4);
    moodLabel_.setBounds(row(14));
    moodBox_.setBounds(row(24));
    left.removeFromTop(4);
    bpmLabel_.setBounds(row(14));
    bpmField_.setBounds(row(24));
    left.removeFromTop(4);
    styleLabel_.setBounds(row(14));
    styleField_.setBounds(left.removeFromTop(56));
    left.removeFromTop(8);
    useSessionTargetButton_.setBounds(left.removeFromTop(26));
    left.removeFromTop(6);
    auto genRow = left.removeFromTop(30);
    if (cancelGenerateButton_.isVisible()) {
        cancelGenerateButton_.setBounds(genRow.removeFromRight(70));
        genRow.removeFromRight(6);
    }
    generateButton_.setBounds(genRow);
    left.removeFromTop(6);
    statusLabel_.setBounds(left.removeFromTop(18));
    left.removeFromTop(4);

    if (ecosystemCoverageBadge_.isVisible()) {
        ecosystemCoverageBadge_.setBounds(left.removeFromTop(20));
        left.removeFromTop(4);
    } else {
        left.removeFromTop(10);
    }

    // Preset row: name field + save button, then load combo
    auto presetRow = left.removeFromTop(26);
    savePresetButton_.setBounds(presetRow.removeFromRight(92));
    presetRow.removeFromRight(4);
    presetNameField_.setBounds(presetRow);
    left.removeFromTop(4);
    presetBox_.setBounds(left.removeFromTop(24));
    left.removeFromTop(6);
    {
        auto pushRow = left.removeFromTop(26);
        if (pluginTarget_ != nullptr) {
            pluginTarget_->setBounds(pushRow.removeFromRight(juce::jmin(200, pushRow.getWidth() / 2)));
            pushRow.removeFromRight(4);
        }
        pushToPluginDspButton_.setBounds(pushRow);
    }
    left.removeFromTop(4);
    mappingReportButton_.setBounds(left.removeFromTop(26));
    left.removeFromTop(4);
    {
        auto traceRow = left.removeFromTop(18);
        decisionTraceButton_.setBounds(traceRow.removeFromLeft(50));
        consensusBadge_.setBounds(traceRow);
    }
    if (consensusChart_.isVisible()) {
        left.removeFromTop(2);
        consensusChart_.setBounds(left.removeFromTop(36));
    }
    left.removeFromTop(4);
    scanPluginsButton_.setBounds(left.removeFromTop(26));
    left.removeFromTop(2);
    libraryBadge_.setBounds(left.removeFromTop(16));
    if (retrySkippedButton_.isVisible()) {
        left.removeFromTop(2);
        retrySkippedButton_.setBounds(left.removeFromTop(KhensuTheme::buttonHSmall));
    }

    // ── Middle column: ecosystem toggles
    auto mid = juce::Rectangle<int>(leftW + 1, top, midW, h - top).reduced(pad);
    for (int i = 0; i < kNumEco; ++i) {
        ecosystemToggles_[i].setBounds(mid.removeFromTop(22));
        mid.removeFromTop(2);
    }

    // ── Right column: results viewport
    int rightX = leftW + midW + 1;
    resultsViewport_.setBounds(rightX, top, w - rightX, h - top);
    layoutResultGrid();
}

void MixerChainPanel::layoutResultGrid()
{
    const int vw = resultsViewport_.getWidth();
    const int n = static_cast<int>(cards_.size());
    int totalH = n * (kCardH + kCardGap) + kCardGap;
    resultsGrid_.setBounds(0, 0, vw, juce::jmax(totalH, resultsViewport_.getHeight()));

    int y = kCardGap;
    for (auto& card : cards_) {
        card->setBounds(kCardGap, y, vw - kCardGap * 2, kCardH);
        y += kCardH + kCardGap;
    }
}

void MixerChainPanel::setStatus(const juce::String& msg, bool isError)
{
    statusLabel_.setText(msg, juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId, isError ? KhensuTheme::accentRed : KhensuTheme::textSecondary);
}

void MixerChainPanel::applySessionTarget()
{
    const auto* session = controller_.getActiveSession();
    if (session == nullptr) {
        setStatus("Start or open a session before loading its target.", true);
        return;
    }

    const auto& target = session->target;
    const auto context = sessionChainContextForTarget(target);
    selectComboText(genreBox_, context.genre);
    selectComboText(moodBox_, context.mood);
    selectComboText(energyBox_, context.energy);
    bpmField_.setText(context.bpm > 0.0 ? juce::String(context.bpm, 1) : juce::String{}, false);

    const auto genre = genreBox_.getText();
    styleField_.setText(TRANS("Session template: ") + genre + " | " + target.mood, false);
    setStatus("Loaded " + genre + " starting-chain context from the active session.");
}

void MixerChainPanel::generate()
{
    if (generating_.load()) return;
    generating_.store(true);
    generateButton_.setEnabled(false);
    setStatus("Generating chain...");

    // Collect inputs on UI thread before launching background thread
    juce::String target = targetBox_.getText().toLowerCase().replace(" ", "_");
    juce::String genre = genreBox_.getText();
    juce::String energy = energyBox_.getText().toLowerCase();
    juce::String mood = moodBox_.getText().toLowerCase();
    double bpm = bpmField_.getText().getDoubleValue();
    if (bpm <= 0.0) bpm = 120.0;
    juce::String styleDesc = styleField_.getText();

    juce::StringArray ecosystems;
    for (int i = 0; i < kNumEco; ++i)
        if (ecosystemToggles_[i].getToggleState()) ecosystems.add(kEcoNames[i]);

    // Snapshot vocal strategy and macros from controller
    juce::String vocalStrategy = controller_.vocalEdit().getSelectedVocalStrategy();
    juce::DynamicObject::Ptr vocalMacrosObj = new juce::DynamicObject();
    for (const char* name : {"Compression", "Presence", "Width", "De-essing", "Saturation", "Reverb", "Delay"}) {
        vocalMacrosObj->setProperty(name, controller_.vocalEdit().getVocalMacroValue(name));
    }
    juce::var vocalMacrosVar = juce::var(vocalMacrosObj.get());

    // Closes the "copilot that learns" loop for chain generation, not just
    // genre-target LUFS/LRA/width/low-end nudging - same signal
    // RecommendationEngine already gets via GenreEntry::adapt().
    double prefLufsOffset = 0.0, prefLraOffset = 0.0, prefWidthOffset = 0.0, prefLowEndOffset = 0.0;
    if (auto* fm = controller_.analysis().getFeedbackManager()) {
        UserPreferenceModel prefs;
        prefs.loadFromDatabase(*fm);
        prefLufsOffset = prefs.getLufsOffset();
        prefLraOffset = prefs.getLraOffset();
        prefWidthOffset = prefs.getStereoWidthOffset();
        prefLowEndOffset = prefs.getLowEndWeightOffset();
    }

    cancelGenerateButton_.setVisible(true);
    resized();
    jobEpoch_ = controller_.sessionEpoch();

    // Capture for background thread
    juce::Component::SafePointer<MixerChainPanel> self(this);

    controller_.runLongInBackground([self, target, genre, energy, mood, bpm, styleDesc, ecosystems, vocalStrategy, vocalMacrosVar,
                 prefLufsOffset, prefLraOffset, prefWidthOffset, prefLowEndOffset]() mutable {
        khensu::ipc::HttpServiceBridge bridge;
        auto result =
            bridge.startMixerChainJob(target, genre, energy, mood, bpm, styleDesc, ecosystems, vocalStrategy,
                                      vocalMacrosVar, prefLufsOffset, prefLraOffset, prefWidthOffset, prefLowEndOffset);

        juce::MessageManager::callAsync([self, result = std::move(result)]() mutable {
            if (!self) return;
            if (result.is_err()) {
                self->generating_.store(false);
                self->generateButton_.setEnabled(true);
                self->cancelGenerateButton_.setVisible(false);
                self->resized();
                self->setStatus(TRANS("Error: ") + juce::String(result.error().message), true);
                return;
            }
            self->currentJobId_ = result.value();
            if (self->controller_.sessionEpoch() != self->jobEpoch_) { // switched while /start was in flight
                self->cancelGeneration();
                self->setStatus(TRANS("Session changed - chain generation cancelled."));
                return;
            }
            self->setStatus(TRANS("Generating chain... (queued)"));
            self->pollJob(self->currentJobId_);
        });
    });
}

void MixerChainPanel::pollJob(const juce::String& jobId)
{
    if (pollStop_) pollStop_->store(true); // a previous wait, if any, ends now
    auto stop = std::make_shared<std::atomic<bool>>(false);
    pollStop_ = stop;
    juce::Component::SafePointer<MixerChainPanel> self(this);
    auto* controller = &controller_;
    const uint64_t epoch = jobEpoch_;
    controller_.runLongInBackground([self, jobId, stop, controller, epoch]() {
        khensu::ipc::HttpServiceBridge bridge;
        using Poller = JobPoller<khensu::ipc::HttpServiceBridge::MixerChainJobStatus>;
        Poller::Hooks hooks;
        hooks.poll = [&bridge](const std::string& id) { return bridge.getMixerChainJobStatus(juce::String(id)); };
        hooks.cancel = [&bridge](const std::string& id) { bridge.cancelMixerChainJob(juce::String(id)); };
        hooks.isTerminal = [](const auto& st) { return !khensu::api::isActiveStatus(st.status); };
        const double started = juce::Time::getMillisecondCounterHiRes();
        hooks.onProgress = [self, jobId, started](const auto& st) {
            const int secs = (int)((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0);
            juce::MessageManager::callAsync([self, jobId, status = st.status, secs]() {
                if (!self || jobId != self->currentJobId_) return;
                self->setStatus(TRANS("Generating chain... (%s, %ns)").replace("%s", status)
                                    .replace("%n", juce::String(secs)));
            });
        };
        // AUDIT-042: a session switch abandons (and cancels) the generation.
        hooks.shouldStop = [stop, controller, epoch] {
            return stop->load() || controller->sessionEpoch() != epoch;
        };
        Poller::Options opts;
        opts.pollIntervalMs = 750;
        opts.deadlineSeconds = 15 * 60;
        const auto out = Poller::run(hooks, opts, jobId.toStdString());

        juce::MessageManager::callAsync([self, jobId, out]() mutable {
            if (!self || jobId != self->currentJobId_) return; // superseded by a cancel/new generate
            self->currentJobId_ = {};
            self->generating_.store(false);
            self->generateButton_.setEnabled(true);
            self->cancelGenerateButton_.setVisible(false);
            self->resized();
            if (self->controller_.sessionEpoch() != self->jobEpoch_) {
                self->setStatus(TRANS("Session changed - chain generation cancelled."));
                return;
            }
            if (!out.finished() || !out.status.has_value()) {
                self->setStatus(TRANS("Chain generation stopped: ") + out.describe(), true);
                return;
            }
            auto& status = *out.status;
            if (status.status == khensu::api::job_status::completed && status.result.has_value()) {
                self->onResultsReady(std::move(status.result->plugins), status.result->decision_hash,
                                     status.result->consensus);
            } else if (status.status == khensu::api::job_status::cancelled) {
                self->setStatus(TRANS("Generation cancelled."));
            } else {
                self->setStatus(TRANS("Error: ")
                                    + (status.error.isNotEmpty() ? status.error : TRANS("generation failed")),
                                true);
            }
        });
    });
}

void MixerChainPanel::cancelGeneration()
{
    if (currentJobId_.isEmpty()) return;
    if (pollStop_) pollStop_->store(true);
    juce::String jobId = currentJobId_;
    currentJobId_ = {};
    generating_.store(false);
    generateButton_.setEnabled(true);
    cancelGenerateButton_.setVisible(false);
    resized();
    setStatus(TRANS("Generation cancelled."));

    controller_.runLongInBackground([jobId]() mutable {
        khensu::ipc::HttpServiceBridge bridge;
        bridge.cancelMixerChainJob(jobId); // fire-and-forget
    });
}

void MixerChainPanel::onResultsReady(std::vector<khensu::ipc::HttpServiceBridge::MixerChainPlugin> plugins,
                                     const juce::String& decisionHash,
                                     std::optional<khensu::ipc::HttpServiceBridge::ConsensusInfo> consensus)
{
    lastPlugins_ = plugins;
    lastDecisionHash_ = decisionHash;
    lastConsensus_ = consensus;
    savePresetButton_.setEnabled(!lastPlugins_.empty());
    pushToPluginDspButton_.setEnabled(!lastPlugins_.empty());
    mappingReportButton_.setEnabled(!lastPlugins_.empty());
    decisionTraceButton_.setEnabled(lastDecisionHash_.isNotEmpty());

    if (lastConsensus_.has_value() && lastConsensus_->agreement != "single") {
        const auto& c = *lastConsensus_;
        juce::Colour badgeColour = c.agreement == "unanimous" ? KhensuTheme::accentTeal
                                   : c.agreement == "high"    ? KhensuTheme::accentBlue
                                                              : KhensuTheme::accentRed;
        consensusBadge_.setText(c.agreement.toUpperCase() + " (" + juce::String(c.providers_responded) + ")",
                                juce::dontSendNotification);
        consensusBadge_.setColour(juce::Label::textColourId, badgeColour);
        consensusBadge_.setTooltip(TRANS("Consensus across ") + juce::String(c.providers_responded) +
                                   " model(s): " + c.providers_queried.joinIntoString(", "));
        consensusBadge_.setVisible(true);
        consensusChart_.info = lastConsensus_;
        consensusChart_.setVisible(true);
        consensusChart_.repaint();
    } else {
        consensusBadge_.setVisible(false);
        consensusChart_.info.reset();
        consensusChart_.setVisible(false);
    }

    // Remove old cards from resultsGrid_
    for (auto& c : cards_)
        resultsGrid_.removeChildComponent(c.get());
    cards_.clear();

    auto setupLabel = [](juce::Label& l, const juce::String& text, float sz, juce::Colour col, bool bold = false) {
        l.setText(text, juce::dontSendNotification);
        l.setFont(juce::Font(juce::FontOptions{sz, bold ? juce::Font::bold : juce::Font::plain}));
        l.setColour(juce::Label::textColourId, col);
    };

    for (size_t i = 0; i < plugins.size() && i < kMaxCards; ++i) {
        auto card = std::make_unique<PluginCard>();
        const auto& p = plugins[i];

        setupLabel(card->nameLabel_, juce::String(p.name), 13.f, KhensuTheme::accentTeal, true);
        setupLabel(card->categoryLabel_, juce::String(p.category), 11.f, KhensuTheme::textPrimary);
        setupLabel(card->ecosystemLabel_, juce::String(p.ecosystem), 10.f, KhensuTheme::accentBlue);
        setupLabel(card->settingsLabel_, juce::String(p.settings_description), 10.f, KhensuTheme::textSecondary);
        card->settingsLabel_.setMinimumHorizontalScale(0.5f);

        resultsGrid_.addAndMakeVisible(*card);
        cards_.push_back(std::move(card));
    }

    // Ecosystem Coverage Badge logic
    int enabledCount = 0;
    for (int i = 0; i < kNumEco; ++i) {
        if (ecosystemToggles_[i].getToggleState()) {
            enabledCount++;
        }
    }
    juce::StringArray uniqueEco;
    for (const auto& p : plugins) {
        if (p.ecosystem.isNotEmpty() && !uniqueEco.contains(p.ecosystem)) {
            uniqueEco.add(p.ecosystem);
        }
    }
    ecosystemCoverageBadge_.setText(TRANS("Ecosystems: ") + juce::String(uniqueEco.size()) + " used / " +
                                        juce::String(enabledCount) + " enabled",
                                    juce::dontSendNotification);
    ecosystemCoverageBadge_.setVisible(!plugins.empty());

    layoutResultGrid();
    resized(); // Reflow the panels vertically to accommodate badge
    setStatus(juce::String(plugins.size()) + " plugins generated.");
    resolveAgainstLibrary();
}

// ────────────────────────────────────────────────────────────────────────────
// "Your Plugins" mode
// ────────────────────────────────────────────────────────────────────────────

void MixerChainPanel::refreshLibraryBadge()
{
    int count = 0;
    if (auto res = controller_.sessions().sessionManager().listScannedPlugins(); res.is_ok())
        count = static_cast<int>(res.value().size());
    libraryBadge_.setText(count > 0 ? "Library: " + juce::String(count) + " plugins scanned"
                                    : "Library: not scanned yet",
                          juce::dontSendNotification);
    // The retry is offered only while something is quarantined and no scan runs.
    const auto held = static_cast<int>(scanner().quarantinedPlugins().size());
    const bool show = held > 0 && !scanner().isScanning();
    if (show)
        retrySkippedButton_.setButtonText(TRANS("Retry Skipped Plugins") + " (" + juce::String(held) + ")");
    if (show != retrySkippedButton_.isVisible()) {
        retrySkippedButton_.setVisible(show);
        resized();
    }
}

mixerchain::PluginScanner& MixerChainPanel::scanner()
{
    if (!pluginScanner_) {
        pluginScanner_ = std::make_unique<mixerchain::PluginScanner>();
        // CORE-07: each plugin is probed in a worker copy of this app, so one
        // that crashes or hangs while loading cannot take the app down.
        pluginScanner_->setWorkerExecutable(
            juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName().toStdString());
    }
    return *pluginScanner_;
}

void MixerChainPanel::retrySkippedPlugins()
{
    if (scanner().isScanning()) return;
    const auto held = static_cast<int>(scanner().quarantinedPlugins().size());
    juce::Component::SafePointer<MixerChainPanel> self(this);
    const bool started = scanner().startRetryQuarantined(
        [self](const std::vector<mixerchain::ScannedPlugin>& found) {
            juce::MessageManager::callAsync([self, found]() {
                if (!self) return;
                self->scanPluginsButton_.setButtonText(TRANS("Scan My Plugins"));
                // Merge into the saved library: the retry found only these.
                auto& sessions = self->controller_.sessions().sessionManager();
                std::vector<mixerchain::ScannedPlugin> merged;
                if (auto lib = sessions.listScannedPlugins(); lib.is_ok()) merged = lib.value();
                for (const auto& p : found)
                    if (std::none_of(merged.begin(), merged.end(), [&](const auto& m) { return m.id == p.id; }))
                        merged.push_back(p);
                if (sessions.saveScannedPlugins(merged).is_err()) {
                    self->setStatus(TRANS("Retry finished but saving the library failed."), true);
                    return;
                }
                self->refreshLibraryBadge();
                juce::String done = TRANS("Retry complete: %n plugins added.")
                                        .replace("%n", juce::String((int)found.size()));
                if (const auto still = self->scanner().quarantinedPlugins().size(); still > 0)
                    done << " " << TRANS("%q still quarantined.").replace("%q", juce::String((int)still));
                self->setStatus(done);
                if (!self->lastPlugins_.empty()) self->resolveAgainstLibrary();
            });
        },
        [self](const std::string& progress) {
            juce::MessageManager::callAsync([self, progress]() {
                if (self) self->setStatus(juce::String(progress));
            });
        });
    if (!started) {
        refreshLibraryBadge();
        return;
    }
    scanPluginsButton_.setButtonText(TRANS("Cancel Scan"));
    retrySkippedButton_.setVisible(false);
    resized();
    setStatus(TRANS("Retrying %n skipped plugins one at a time - answer any licence or sign-in window that "
                    "opens (up to 5 minutes each).")
                  .replace("%n", juce::String(held)));
}

void MixerChainPanel::scanPlugins()
{
    if (scanner().isScanning()) {
        scanner().cancelScan();
        scanPluginsButton_.setButtonText(TRANS("Scan My Plugins"));
        setStatus("Plugin scan cancelled.");
        refreshLibraryBadge();
        return;
    }
    retrySkippedButton_.setVisible(false);
    resized();

    scanPluginsButton_.setButtonText(TRANS("Cancel Scan"));
    setStatus("Scanning plugin library...");

    // Each plugin is instantiated in a worker process (CORE-07), so a badly
    // behaved one is quarantined instead of crashing the app. Both callbacks
    // fire on scan threads and must hop to the message thread.
    juce::Component::SafePointer<MixerChainPanel> self(this);
    scanner().startScan(
        [self](const std::vector<mixerchain::ScannedPlugin>& found) {
            juce::MessageManager::callAsync([self, found]() {
                if (!self) return;
                self->scanPluginsButton_.setButtonText(TRANS("Scan My Plugins"));
                auto saved = self->controller_.sessions().sessionManager().saveScannedPlugins(found);
                if (saved.is_err()) {
                    self->setStatus("Scan finished but saving the library failed.", true);
                    return;
                }
                self->refreshLibraryBadge();
                juce::String done = TRANS("Scan complete: %n plugins found.").replace("%n", juce::String((int)found.size()));
                // The per-plugin "Skipped" lines scroll past; say how many are held back.
                if (self->pluginScanner_ != nullptr)
                    if (const auto held = self->pluginScanner_->quarantinedPlugins().size(); held > 0)
                        done << " " << TRANS("%q quarantined (crashed or hung while loading).")
                                           .replace("%q", juce::String((int)held));
                self->setStatus(done);
                if (!self->lastPlugins_.empty()) self->resolveAgainstLibrary(); // re-annotate the visible chain
            });
        },
        [self](const std::string& progress) {
            juce::MessageManager::callAsync([self, progress]() {
                if (self) self->setStatus(juce::String(progress));
            });
        });
}

void MixerChainPanel::resolveAgainstLibrary()
{
    if (cards_.empty() || lastPlugins_.empty()) return;

    // If the library was never scanned there is nothing to match against -
    // point at the scan button instead of firing a request that matches nothing.
    if (auto lib = controller_.sessions().sessionManager().listScannedPlugins(); lib.is_err() || lib.value().empty()) {
        for (auto& card : cards_) {
            card->matchLabel_.setText(TRANS("Scan your plugins to see local matches"), juce::dontSendNotification);
            card->matchLabel_.setFont(juce::Font(juce::FontOptions{10.f, juce::Font::italic}));
            card->matched_ = false;
            card->applyColours();
        }
        return;
    }

    // Map each generated slot's free-form category to matcher.py's generic
    // effect vocabulary; remember which card each request row belongs to.
    juce::StringArray generics;
    std::vector<int> cardForRow;
    for (size_t i = 0; i < lastPlugins_.size() && i < cards_.size(); ++i) {
        auto generic = genericEffectForCategory(lastPlugins_[i].category, lastPlugins_[i].name);
        if (generic.isEmpty()) {
            cards_[i]->matchLabel_.setText({}, juce::dontSendNotification);
            continue;
        }
        generics.add(generic);
        cardForRow.push_back(static_cast<int>(i));
    }
    if (generics.isEmpty()) return;

    juce::Component::SafePointer<MixerChainPanel> self(this);
    controller_.runLongInBackground([self, generics, cardForRow]() mutable {
        khensu::ipc::HttpServiceBridge bridge;
        auto result = bridge.resolveChain(generics);
        juce::MessageManager::callAsync([self, cardForRow, result = std::move(result)]() mutable {
            if (!self) return;
            if (result.is_err()) return; // best-effort annotation; generation result stands
            const auto& resolved = result.value();

            int matched = 0;
            juce::StringArray gaps;
            for (size_t row = 0; row < resolved.size() && row < cardForRow.size(); ++row) {
                const auto cardIdx = static_cast<size_t>(cardForRow[row]);
                if (cardIdx >= self->cards_.size()) continue;
                auto& card = *self->cards_[cardIdx];
                auto& label = card.matchLabel_;
                if (resolved[row].matched_plugin.has_value()) {
                    const auto& m = *resolved[row].matched_plugin;
                    label.setText(TRANS("Yours: %n (%f)").replace("%n", m.name).replace("%f", m.format),
                                  juce::dontSendNotification);
                    label.setFont(juce::Font(juce::FontOptions{10.f, juce::Font::bold}));
                    card.matched_ = true;
                    ++matched;
                } else {
                    label.setText(TRANS("No local match - use built-in DSP or the suggested ecosystem"),
                                  juce::dontSendNotification);
                    label.setFont(juce::Font(juce::FontOptions{10.f, juce::Font::italic}));
                    card.matched_ = false;
                    gaps.add(resolved[row].effect);
                }
                card.applyColours(); // live theme colours, re-applied on theme change (UIX-22)
            }

            juce::String summary = TRANS("%n plugins generated. Your library covers %m/%t slots")
                                       .replace("%n", juce::String((int)self->lastPlugins_.size()))
                                       .replace("%m", juce::String(matched))
                                       .replace("%t", juce::String((int)cardForRow.size()));
            if (!gaps.isEmpty()) summary += "  -  " + TRANS("missing:") + " " + gaps.joinIntoString(", ");
            self->setStatus(summary);
        });
    });
}

// ────────────────────────────────────────────────────────────────────────────
// Presets
// ────────────────────────────────────────────────────────────────────────────

juce::File MixerChainPanel::presetDirectory()
{
    auto dir = juce::File(juce::String(AppPaths::dataDirectory())).getChildFile("chain_presets");
    if (!dir.isDirectory()) dir.createDirectory();
    return dir;
}

void MixerChainPanel::savePreset()
{
    if (lastPlugins_.empty()) {
        setStatus("Generate a chain before saving a preset.", true);
        return;
    }

    juce::String name = presetNameField_.getText().trim();
    if (name.isEmpty()) name = "Chain " + juce::Time::getCurrentTime().formatted("%Y-%m-%d %H%M%S");
    name = juce::File::createLegalFileName(name);

    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("schema_version", 1);
    obj->setProperty("name", name);
    obj->setProperty("target", targetBox_.getText());
    obj->setProperty("genre", genreBox_.getText());
    obj->setProperty("energy", energyBox_.getText());
    obj->setProperty("mood", moodBox_.getText());
    obj->setProperty("bpm", bpmField_.getText().getDoubleValue());
    obj->setProperty("style", styleField_.getText());

    juce::Array<juce::var> ecoArr;
    for (int i = 0; i < kNumEco; ++i)
        if (ecosystemToggles_[i].getToggleState()) ecoArr.add(juce::String(kEcoNames[i]));
    obj->setProperty("ecosystems", juce::var(ecoArr));

    juce::Array<juce::var> pluginArr;
    for (const auto& p : lastPlugins_) {
        juce::DynamicObject::Ptr pObj = new juce::DynamicObject();
        pObj->setProperty("name", p.name);
        pObj->setProperty("category", p.category);
        pObj->setProperty("ecosystem", p.ecosystem);
        pObj->setProperty("settings_description", p.settings_description);
        pluginArr.add(juce::var(pObj.get()));
    }
    obj->setProperty("plugins", juce::var(pluginArr));

    // Preset Schema Extension: Save exact corrective DSP parameter states
    std::vector<std::string> mappedStages;
    auto params = mixerchain::approximateDspParams(lastPlugins_, &mappedStages);
    juce::DynamicObject::Ptr paramsObj = new juce::DynamicObject();
    for (const auto& [k, v] : params) {
        paramsObj->setProperty(juce::String(k), v);
    }
    obj->setProperty("parameter_states", juce::var(paramsObj.get()));

    auto file = presetDirectory().getChildFile(name + ".json");

    auto writePreset = [this, file, obj, name]() {
        if (file.replaceWithText(juce::JSON::toString(juce::var(obj.get()), false))) {
            setStatus("Preset saved: " + name);
            refreshPresetList();
            presetBox_.setText(name, juce::dontSendNotification);
        } else {
            setStatus("Could not write preset file.", true);
        }
    };

    if (file.existsAsFile()) {
        juce::AlertWindow::showOkCancelBox(juce::AlertWindow::WarningIcon, "Overwrite Preset?",
                                           "A preset named '" + name + "' already exists. Do you want to overwrite it?",
                                           "Overwrite", "Cancel", nullptr,
                                           juce::ModalCallbackFunction::create([writePreset](int result) {
                                               if (result == 1) { // OK
                                                   writePreset();
                                               }
                                           }));
    } else {
        writePreset();
    }
}

void MixerChainPanel::loadPreset(const juce::File& file)
{
    auto json = juce::JSON::parse(file.loadFileAsString());
    auto* obj = json.getDynamicObject();
    if (obj == nullptr) {
        setStatus("Preset file is not valid JSON: " + file.getFileName(), true);
        return;
    }

    loadedPresetParams_.clear();

    // Restore generation context (each combo falls back to its current value
    // when the stored text no longer matches an item - old presets stay loadable).
    auto selectByText = [](juce::ComboBox& box, const juce::String& text) {
        for (int i = 0; i < box.getNumItems(); ++i) {
            if (box.getItemText(i).equalsIgnoreCase(text)) {
                box.setSelectedItemIndex(i, juce::dontSendNotification);
                return;
            }
        }
    };
    selectByText(targetBox_, obj->getProperty("target").toString());
    selectByText(genreBox_, obj->getProperty("genre").toString());
    selectByText(energyBox_, obj->getProperty("energy").toString());
    selectByText(moodBox_, obj->getProperty("mood").toString());

    const double bpm = static_cast<double>(obj->getProperty("bpm"));
    bpmField_.setText(bpm > 0.0 ? juce::String(juce::roundToInt(bpm)) : juce::String{}, false);
    styleField_.setText(obj->getProperty("style").toString(), false);

    if (auto* ecoArr = obj->getProperty("ecosystems").getArray()) {
        for (int i = 0; i < kNumEco; ++i) {
            bool enabled = false;
            for (const auto& e : *ecoArr)
                if (e.toString().equalsIgnoreCase(kEcoNames[i])) {
                    enabled = true;
                    break;
                }
            ecosystemToggles_[i].setToggleState(enabled, juce::dontSendNotification);
        }
    }

    // Extract exact parameter states if they exist
    if (auto* paramsObj = obj->getProperty("parameter_states").getDynamicObject()) {
        for (const auto& p : paramsObj->getProperties()) {
            loadedPresetParams_[p.name.toString()] = static_cast<float>(p.value);
        }
    }

    std::vector<khensu::ipc::HttpServiceBridge::MixerChainPlugin> plugins;
    if (auto* pluginArr = obj->getProperty("plugins").getArray()) {
        for (const auto& pv : *pluginArr) {
            khensu::ipc::HttpServiceBridge::MixerChainPlugin p;
            p.name = pv["name"].toString();
            p.category = pv["category"].toString();
            p.ecosystem = pv["ecosystem"].toString();
            p.settings_description = pv["settings_description"].toString();
            if (p.name.isNotEmpty()) plugins.push_back(std::move(p));
        }
    }

    onResultsReady(std::move(plugins));

    // Auto-post exact parameters to the plugin for byte-identical load state
    if (!loadedPresetParams_.empty()) {
        auto params = loadedPresetParams_;
        controller_.runLongInBackground([params, target = controller_.lifecycle().pluginTargetInstance()]() {
            khensu::ipc::HttpServiceBridge bridge;
            bridge.postPluginParams(params, target); // PLG-04: the chosen instance ("" = all)
        });
    }

    setStatus("Preset loaded: " + file.getFileNameWithoutExtension());
}

void MixerChainPanel::refreshPresetList()
{
    presetBox_.clear(juce::dontSendNotification);
    int id = 1;
    for (const auto& f : presetDirectory().findChildFiles(juce::File::findFiles, false, "*.json"))
        presetBox_.addItem(f.getFileNameWithoutExtension(), id++);
}

void MixerChainPanel::pushToPluginDsp()
{
    // WF-9: this chain is a preset for the plugin chain, not a push of its
    // own. It is staged in the one model and applied from the one editor
    // (Mix / Master's Signal Flow Rack).
    if (lastPlugins_.empty()) {
        setStatus("Generate or load a chain first.", true);
        return;
    }
    std::map<juce::String, float> juceParams;
    std::vector<std::string> mappedStages;
    if (!loadedPresetParams_.empty()) {
        juceParams = loadedPresetParams_;
        for (const auto& [k, v] : juceParams)
            mappedStages.push_back(k.toStdString());
    } else {
        for (const auto& [id, value] : mixerchain::approximateDspParams(lastPlugins_, &mappedStages))
            juceParams[juce::String(id)] = value;
    }
    // An empty map is "nothing maps to the built-in DSP", not a failure (UIX-19).
    if (juceParams.empty()) {
        setStatus(TRANS("No stages in this chain map to the plugin's built-in DSP."));
        return;
    }
    controller_.lifecycle().stagePluginChain(juceParams, TRANS("Mixer Chain"));
    juce::StringArray stagesArr;
    for (const auto& st : mappedStages)
        stagesArr.add(juce::String(st));
    setStatus(TRANS("Staged in the plugin chain (%s): apply it on Mix / Master's Signal Flow Rack.")
                  .replace("%s", stagesArr.joinIntoString(", ")));
    controller_.shell().showScreen(ScreenType::MixMaster);
}

void MixerChainPanel::showDecisionTrace()
{
    if (lastDecisionHash_.isEmpty()) return;
    juce::String body = "Decision hash: " + lastDecisionHash_ +
                        "\n\nThis fingerprint identifies exactly which model/provider and inputs produced "
                        "this chain. It's attached to any render applied from this chain via decision_hash "
                        "so the render can be traced back to the decision that caused it.";

    if (lastConsensus_.has_value()) {
        const auto& c = *lastConsensus_;
        if (c.agreement == "single") {
            body += "\n\nSingle-provider generation - set KHENSU_LLM_CONSENSUS_PROVIDERS to "
                    "2+ providers to cross-check this decision against another model.";
        } else {
            body += "\n\nConsensus: " + c.agreement.toUpperCase();
            if (c.agreement_score.has_value())
                body += " (" + juce::String(juce::roundToInt(*c.agreement_score * 100.0)) + "% agreement)";
            body += "\nQueried " + juce::String(c.providers_queried.size()) + " provider(s), " +
                    juce::String(c.providers_responded) + " responded: " + c.providers_queried.joinIntoString(", ");
            if (!c.alternative_descriptions.isEmpty()) {
                body += "\n\nWhat the other model(s) proposed instead:";
                for (const auto& alt : c.alternative_descriptions)
                    body += "\n    - " + alt;
            }
        }
    }

    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Decision Trace", body, "Close");
}

void MixerChainPanel::showMappingReport()
{
    if (lastPlugins_.empty()) {
        setStatus("Generate or load a chain first.", true);
        return;
    }

    std::vector<std::string> mappedStages;
    auto params = mixerchain::approximateDspParams(lastPlugins_, &mappedStages);

    juce::String report;
    report << "=== MIXER CHAIN DSP MAPPING REPORT ===\n\n";
    report << "This report shows how the generated third-party plugins map to Khensu's built-in corrective DSP "
              "blocks:\n\n";

    for (const auto& p : lastPlugins_) {
        report << "- " << p.name << " (" << p.category << ")\n";
        report << "  Raw Recipe: \"" << p.settings_description << "\"\n";

        juce::StringArray mappings;
        if (p.category.containsIgnoreCase("Compressor")) {
            mappings.add("Compressor Bypass = Off");
            if (params.count(mixerchain::DspParamIds::compThreshDb))
                mappings.add("Threshold = " + juce::String(params[mixerchain::DspParamIds::compThreshDb], 1) + " dB");
            if (params.count(mixerchain::DspParamIds::compRatio))
                mappings.add("Ratio = " + juce::String(params[mixerchain::DspParamIds::compRatio], 1) + ":1");
        } else if (p.category.containsIgnoreCase("EQ")) {
            mappings.add("EQ Bypass = Off");
            if (params.count(mixerchain::DspParamIds::eqFreq))
                mappings.add("Freq = " + juce::String(juce::roundToInt(params[mixerchain::DspParamIds::eqFreq])) + " Hz");
            if (params.count(mixerchain::DspParamIds::eqGainDb))
                mappings.add("Gain = " + juce::String(params[mixerchain::DspParamIds::eqGainDb], 1) + " dB");
        } else if (p.category.containsIgnoreCase("Limiter")) {
            mappings.add("Limiter Bypass = Off");
            if (params.count(mixerchain::DspParamIds::limThreshDb))
                mappings.add("Ceiling = " + juce::String(params[mixerchain::DspParamIds::limThreshDb], 1) + " dB");
        } else if (p.settings_description.contains("HPF")) {
            mappings.add("HPF Bypass = Off");
            if (params.count(mixerchain::DspParamIds::hpfFreq))
                mappings.add("HPF Freq = " + juce::String(juce::roundToInt(params[mixerchain::DspParamIds::hpfFreq])) + " Hz");
        } else if (p.settings_description.contains("LPF")) {
            mappings.add("LPF Bypass = Off");
            if (params.count(mixerchain::DspParamIds::lpfFreq))
                mappings.add("LPF Freq = " + juce::String(juce::roundToInt(params[mixerchain::DspParamIds::lpfFreq])) + " Hz");
        }

        // Saturation / stereo-width / bass-mono / transient-shaping roles have
        // no real-time equivalent in the built-in plugin chain (it only has
        // HPF/LPF/EQ/Compressor/Limiter) - they're real, but live in the
        // separate Apply Chain render pipeline (GlueCompressor, TapeSaturation,
        // StereoWidener, BassMonoMaker, TransientShaper). The old generic
        // "(None / Ext-Hosted Only)" message read as "this recipe role is
        // unsupported," which was misleading - it's supported, just via a
        // different, non-real-time path.
        juce::String unavailableNote;
        if (mappings.isEmpty()) {
            if (p.category.containsIgnoreCase("Saturation") || p.category.containsIgnoreCase("Tape"))
                unavailableNote = "Not in the built-in real-time chain - render it via Apply Chain's Tape Saturation "
                                  "effect instead (Mix/Master Engine > Apply Chain).";
            else if (p.category.containsIgnoreCase("Stereo") || p.category.containsIgnoreCase("Widener"))
                unavailableNote = "Not in the built-in real-time chain - render it via Apply Chain's Stereo Widener "
                                  "effect instead (Mix/Master Engine > Apply Chain).";
            else if (p.category.containsIgnoreCase("Bass Mono") || p.category.containsIgnoreCase("Mono Maker"))
                unavailableNote = "Not in the built-in real-time chain - render it via Apply Chain's Bass Mono Maker "
                                  "effect instead (Mix/Master Engine > Apply Chain).";
            else if (p.category.containsIgnoreCase("Transient"))
                unavailableNote = "Not in the built-in real-time chain - render it via Apply Chain's Transient Shaper "
                                  "effect instead (Mix/Master Engine > Apply Chain).";
        }

        if (unavailableNote.isNotEmpty()) {
            report << "  -> " << unavailableNote << "\n\n";
        } else if (mappings.isEmpty()) {
            report << "  -> Mapped to: (None / Ext-Hosted Only)\n\n";
        } else {
            report << "  -> Mapped to built-in DSP: " << mappings.joinIntoString(", ") << "\n\n";
        }
    }

    juce::AlertWindow::showOkCancelBox(juce::AlertWindow::InfoIcon, "DSP Mapping Report", report, "Close",
                                       "Force Manual Sync", nullptr,
                                       juce::ModalCallbackFunction::create([this](int result) {
                                           if (result == 0) { // Force Manual Sync
                                               pushToPluginDsp();
                                           }
                                       }));
}

} // namespace khensu
