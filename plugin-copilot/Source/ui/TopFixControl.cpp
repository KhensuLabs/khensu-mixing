// plugin-copilot/Source/ui/TopFixControl.cpp
#include "TopFixControl.h"
#include <khensu/mixerchain/RecommendationDspMapper.h>
#include <khensu/ui/KhensuPalette.h>
#include <khensu/ui/StudioStyle.h>
#include "ButtonRoles.h"
#include "../PluginProcessor.h"

#include <algorithm>

namespace khensu {

namespace copilot {
std::vector<Recommendation> topFindings(const std::vector<Recommendation>& recs, const IssueStateLookup& stateOf,
                                        size_t limit)
{
    std::vector<Recommendation> open;
    for (const auto& rec : recs) {
        if (rec.severity <= 0.0f) continue;
        if (stateOf && stateOf(rec.id) != issue_state::State::Open) continue;
        open.push_back(rec);
    }
    std::stable_sort(open.begin(), open.end(),
                     [](const Recommendation& a, const Recommendation& b) { return a.severity > b.severity; });
    if (open.size() > limit) open.resize(limit);
    return open;
}

std::optional<Recommendation> selectTopFix(const std::vector<Recommendation>& recs, const IssueStateLookup& stateOf)
{
    const auto none = [](const std::string&) { return 0.0f; }; // every band free
    for (const auto& rec : topFindings(recs, stateOf, recs.size()))
        if (!mixerchain::mapRecommendationToDsp(rec, 1.0f, none).mappedKeys.empty()) return rec;
    return std::nullopt;
}
} // namespace copilot

namespace {
juce::String utf8(const std::string& s) { return juce::String(juce::CharPointer_UTF8(s.c_str())); }
} // namespace

TopFixControl::TopFixControl(KhensuCopilotProcessor& processor) : processor_(processor), applyButton_(TRANS("Apply top fix"))
{
    setTitle(TRANS("Top fix"));
    titleLabel_.setText(TRANS("TOP FIX"), juce::dontSendNotification);
    titleLabel_.setFont(ui::studio::labelFont());
    titleLabel_.setColour(juce::Label::textColourId, ui::Palette::textMuted);
    titleLabel_.setAccessible(false);
    addAndMakeVisible(titleLabel_);

    findingLabel_.setFont(ui::Palette::uiFont(ui::studio::kBodySize, juce::Font::bold));
    findingLabel_.setColour(juce::Label::textColourId, ui::Palette::textPrimary);
    findingLabel_.setMinimumHorizontalScale(0.85f);
    findingLabel_.setTitle(TRANS("Top fix finding"));
    addAndMakeVisible(findingLabel_);

    reasonLabel_.setFont(ui::Palette::uiFont(ui::studio::kLabelSize));
    reasonLabel_.setColour(juce::Label::textColourId, ui::Palette::textMuted);
    reasonLabel_.setTitle(TRANS("Top fix status"));
    addAndMakeVisible(reasonLabel_);

    applyButton_.setComponentID("applyTopFixButton");
    applyButton_.setTitle(TRANS("Apply top fix"));
    applyButton_.setTooltip(TRANS("Apply the most severe open finding's fix to the built-in chain, live. "
                                  "Turn Amount to scale it; Undo puts every parameter back."));
    copilot::button_role::markPrimary(applyButton_);
    applyButton_.onClick = [this] {
        if (topFix_.has_value() && onApply) onApply(*topFix_);
        updateTexts();
    };
    addAndMakeVisible(applyButton_);

    amountKnob_.setComponentID("fixAmountKnob");
    amountKnob_.setTitle(TRANS("Fix amount"));
    amountKnob_.setTooltip(TRANS("How much of the fix to apply (automatable). Moving it re-applies the live fix; "
                                 "0 % puts the touched parameters back. Alt/Option-click returns to 100 %.")
                           + " " + ui::KhensuKnob::usageHint());
    if (auto* parameter = processor_.getAPVTS().getParameter(KhensuCopilotProcessor::ParamIDs::fixAmount))
        amountAttachment_ = std::make_unique<juce::SliderParameterAttachment>(*parameter, amountKnob_);
    // After the attachment, which installs the parameter's own text functions.
    amountKnob_.setDefaultValue(1.0);
    amountKnob_.setUnit(ui::KhensuKnob::Unit::Percent);
    addAndMakeVisible(amountKnob_);
    updateTexts();
}

TopFixControl::~TopFixControl() = default;

void TopFixControl::setRecommendations(const std::vector<Recommendation>& recs, copilot::IssueStateLookup stateOf)
{
    topFix_ = copilot::selectTopFix(recs, stateOf);
    updateTexts();
}

void TopFixControl::refresh()
{
    updateTexts();
}

void TopFixControl::setCompact(bool compact)
{
    compact_ = compact;
    titleLabel_.setVisible(!compact);
    resized();
}

void TopFixControl::updateTexts()
{
    applyButton_.setEnabled(topFix_.has_value());
    if (!topFix_.has_value()) {
        findingLabel_.setText(TRANS("No open finding the built-in chain can fix"), juce::dontSendNotification);
        reasonLabel_.setText(TRANS("Load a session to see its findings."), juce::dontSendNotification);
        return;
    }
    findingLabel_.setText(utf8(topFix_->what), juce::dontSendNotification);
    findingLabel_.setTooltip(utf8(topFix_->suggested_action));
    juce::String reason;
    if (processor_.hasLiveFix() && processor_.getLiveFixId() == topFix_->id)
        reason = TRANS("Live - Amount re-applies it as you turn it.");
    else if (auto blocked = KhensuCopilotProcessor::autoApplyBlockReason(*topFix_); blocked.isNotEmpty())
        reason = blocked;
    else
        reason = utf8(topFix_->suggested_action);
    if (reasonLabel_.getText() != reason) reasonLabel_.setText(reason, juce::dontSendNotification);
}

void TopFixControl::paint(juce::Graphics& g)
{
    g.setColour(ui::studio::surface());
    g.fillRoundedRectangle(getLocalBounds().toFloat(), ui::studio::kRadius);
}

void TopFixControl::resized()
{
    auto b = getLocalBounds().reduced(ui::studio::kGap, 4);
    const int knobW = juce::jmin(64, b.getHeight());
    amountKnob_.setBounds(b.removeFromRight(knobW));
    b.removeFromRight(ui::studio::kGap);
    applyButton_.setBounds(b.removeFromRight(118).withSizeKeepingCentre(118, 32));
    b.removeFromRight(ui::studio::kGap);
    if (!compact_) titleLabel_.setBounds(b.removeFromTop(14));
    const int rows = b.getHeight() >= 34 ? 2 : 1;
    findingLabel_.setBounds(b.removeFromTop(b.getHeight() / rows));
    reasonLabel_.setVisible(rows == 2);
    reasonLabel_.setBounds(b);
}

//==============================================================================
TopFindingsList::TopFindingsList()
{
    setTitle(TRANS("Top findings"));
}

void TopFindingsList::setFindings(std::vector<Recommendation> findings)
{
    findings_ = std::move(findings);
    juce::StringArray lines;
    for (const auto& f : findings_) lines.add(utf8(f.what));
    setDescription(lines.joinIntoString("; "));
    repaint();
}

void TopFindingsList::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().reduced(ui::studio::kGap, 2);
    g.setFont(ui::studio::labelFont());
    g.setColour(ui::Palette::textMuted);
    g.drawText(TRANS("TOP FINDINGS"), b.removeFromTop(14), juce::Justification::centredLeft);
    if (findings_.empty()) {
        g.drawText(TRANS("No open findings - load a session from Khensu Mixing."), b, juce::Justification::centredLeft);
        return;
    }
    const int rowH = juce::jmax(16, juce::jmin(22, b.getHeight() / 3));
    for (const auto& f : findings_) {
        auto row = b.removeFromTop(rowH);
        if (row.getHeight() < 12) break;
        // Severity colours mean severity only (red / amber / blue).
        const auto colour = f.severity >= 70.0f   ? ui::Palette::statusBad
                            : f.severity >= 40.0f ? ui::Palette::statusWarn
                                                  : ui::Palette::accentBlue;
        g.setColour(colour);
        g.fillEllipse(row.removeFromLeft(10).toFloat().withSizeKeepingCentre(7.0f, 7.0f));
        row.removeFromLeft(6);
        g.setColour(ui::Palette::textPrimary);
        g.setFont(ui::Palette::uiFont(ui::studio::kBodySize));
        g.drawText(utf8(f.what), row, juce::Justification::centredLeft, true);
    }
}

} // namespace khensu
