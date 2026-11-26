// desktop-app/Source/ui/shared/RecommendationCard.cpp
#include "RecommendationCard.h"
#include "KhensuTheme.h"
#include "KhensuLookAndFeel.h"
#include "FindingCalibrationCache.h"
#include "../../session/RecommendationChain.h"
#include <cmath>
#if !KHENSU_TESTS
#include "../../IAppController.h"
#include "CounterfactualView.h"
#endif
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/systembrain/SystemBrainDatabase.h>

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void RecommendationCard::applyThemeColours()
{
    learnMoreButton_.setColour(juce::HyperlinkButton::textColourId, KhensuTheme::accentTeal);
}

RecommendationCard::RecommendationCard(IAppController& controller) : controller_(controller)
{
    applyThemeColours();
#if !KHENSU_TESTS
    // CE-TRU-1: which finding codes are calibrated (fetched once, app-wide).
    FindingCalibrationCache::ensureFetched(
        [this](std::function<void()> job) { controller_.runInBackground(std::move(job)); });
#endif
    applyButton_.onClick = [this]() {
        if (appliesByRendering()) {
            onApplyClicked(rec_); // the owner calls setApplyInFlight / setApplyResult
            return;
        }
        // Advice-only: nothing renders, so this only marks the finding handled.
        if (onMarkDone) onMarkDone(rec_.id);
        applyButton_.setAlpha(1.0f);
        dismissButton_.setAlpha(0.4f);
    };
    updateApplyButton();
    addAndMakeVisible(applyButton_);
    applyButton_.setVisible(false);

    dismissButton_.setButtonText(TRANS("Dismiss"));
    dismissButton_.onClick = [this]() {
        juce::PopupMenu menu;
        menu.addItem(1, "Too bright");
        menu.addItem(2, "Too squashed");
        menu.addItem(3, "Too dynamic");
        menu.addItem(4, "Not needed / personal taste");
        menu.showMenuAsync(juce::PopupMenu::Options{}, [this](int result) {
            if (result > 0) {
                if (onFeedbackClicked) {
                    onFeedbackClicked(rec_.id, false);
                }
                this->setVisible(false);
                if (auto* parent = getParentComponent()) parent->resized();
            }
        });
    };
    addAndMakeVisible(dismissButton_);
    dismissButton_.setVisible(false);

    juce::Font linkFont = KhensuTheme::bodyFont(type::caption);
    linkFont.setUnderline(true);
    learnMoreButton_.setFont(linkFont, false);
    learnMoreButton_.setVisible(false);
    addChildComponent(learnMoreButton_);

    tweakButton_.setButtonText(TRANS("Tweak"));
    tweakButton_.onClick = [this]() {
        tweaking_ = !tweaking_;

        freqSlider_.setVisible(tweaking_);
        freqLabel_.setVisible(tweaking_);
        gainSlider_.setVisible(tweaking_);
        gainLabel_.setVisible(tweaking_);
        qSlider_.setVisible(tweaking_);
        qLabel_.setVisible(tweaking_);

        if (auto* parent = getParentComponent()) {
            parent->resized();
        }
        resized();
        repaint();
    };
    addAndMakeVisible(tweakButton_);
    tweakButton_.setVisible(false);

    reasoningButton_.setButtonText(TRANS("Reasoning"));
    reasoningButton_.onClick = [this]() {
#if !KHENSU_TESTS
        controller_.shell().showChainReasoning(rec_);
#endif
    };
    addAndMakeVisible(reasoningButton_);
    reasoningButton_.setVisible(false);

    // CE-TRU-2: the smallest verified change that clears this finding.
    whatFixesButton_.setButtonText(TRANS("What would fix this?"));
    whatFixesButton_.setTooltip(TRANS("Find the smallest single change that clears this finding, "
                                      "rendered and re-measured, with Audition and Apply"));
    whatFixesButton_.onClick = [this]() {
#if !KHENSU_TESTS
        juce::CallOutBox::launchAsynchronously(std::make_unique<CounterfactualView>(controller_, rec_),
                                               whatFixesButton_.getScreenBounds(), nullptr);
#endif
    };
    addAndMakeVisible(whatFixesButton_);
    whatFixesButton_.setVisible(false);

    setupSliders();
}

void RecommendationCard::setupSliders()
{
    freqSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    freqSlider_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 50, 15);
    addAndMakeVisible(freqSlider_);
    freqSlider_.setVisible(false);
    freqLabel_.setText(TRANS("Freq (Hz)"), juce::dontSendNotification);
    freqLabel_.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
    freqLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    addAndMakeVisible(freqLabel_);
    freqLabel_.setVisible(false);

    gainSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    gainSlider_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 50, 15);
    addAndMakeVisible(gainSlider_);
    gainSlider_.setVisible(false);
    gainLabel_.setText(TRANS("Gain (dB)"), juce::dontSendNotification);
    gainLabel_.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
    gainLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    addAndMakeVisible(gainLabel_);
    gainLabel_.setVisible(false);

    qSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    qSlider_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 50, 15);
    addAndMakeVisible(qSlider_);
    qSlider_.setVisible(false);
    qLabel_.setText(TRANS("Q"), juce::dontSendNotification);
    qLabel_.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
    qLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    addAndMakeVisible(qLabel_);
    qLabel_.setVisible(false);

    freqSlider_.onValueChange = [this]() {
        rec_.suggested_parameters["eq_freq_hz"] = (float)freqSlider_.getValue();
        repaint();
    };
    gainSlider_.onValueChange = [this]() {
        rec_.suggested_parameters["eq_gain_db"] = (float)gainSlider_.getValue();
        repaint();
    };
    qSlider_.onValueChange = [this]() {
        rec_.suggested_parameters["eq_q"] = (float)qSlider_.getValue();
        repaint();
    };
}

void RecommendationCard::setPersonalReason(const juce::String& reason)
{
    personalReason_ = reason.trim();
    updateDetailText();
    repaint();
}

void RecommendationCard::updateDetailText()
{
    detailText_ = baseDetailText_;
    if (personalReason_.isNotEmpty())
        detailText_ = TRANS("Ranked for you because") + " " + personalReason_ + "." +
                      (detailText_.isNotEmpty() ? "\n\n" + detailText_ : juce::String());
    // Cards are laid out before paint(). This conservative text-length
    // estimate prevents evidence/caveats from being silently clipped while
    // retaining the compact default for ordinary recommendations.
    // Evidence lines (CE-TRU-3) are short but each takes a row of its own.
    const int explicitLines = juce::StringArray::fromLines(detailText_).size();
    const int estimatedLines = juce::jlimit(4, 16, juce::jmax(detailText_.length() / 72, explicitLines) + 2);
    detailHeight_ = juce::jmax(120, estimatedLines * 14 + 42);
}

void RecommendationCard::updateApplyButton()
{
    // The label depends on the owner's hook, which is set after construction,
    // so this runs again whenever the card expands.
    applyButton_.setEnabled(applyState_ != ApplyState::Rendering);
    if (applyState_ == ApplyState::Rendering) {
        applyButton_.setButtonText(TRANS("Rendering..."));
    } else if (applyState_ == ApplyState::Applied) {
        applyButton_.setButtonText(TRANS("Applied"));
    } else if (appliesByRendering()) {
        applyButton_.setButtonText(TRANS("Apply"));
        applyButton_.setTooltip(TRANS("Render this fix onto the mix and load it as the B side to A/B."));
    } else {
        applyButton_.setButtonText(TRANS("Mark done"));
        applyButton_.setTooltip(TRANS("Nothing here can be rendered automatically - dial it in yourself, then mark "
                                      "it done."));
    }
}

void RecommendationCard::setApplyInFlight(bool inFlight)
{
    applyState_ = inFlight ? ApplyState::Rendering : ApplyState::Idle;
    updateApplyButton();
}

void RecommendationCard::setApplyResult(bool rendered)
{
    applyState_ = rendered ? ApplyState::Applied : ApplyState::Idle;
    updateApplyButton();
    if (rendered) {
        applyButton_.setAlpha(1.0f);
        dismissButton_.setAlpha(0.4f);
    }
}

void RecommendationCard::setRecommendation(const Recommendation& rec)
{
    rec_ = rec;
    tweaking_ = false;
    baseDetailText_ = evidenceSummaryFor(rec_);
    issueCode_ = issueCodeFor(rec_);
    setTooltip(FindingCalibrationCache::basisTooltip(issueCode_));
    if (!rec_.why_helps.empty()) {
        if (baseDetailText_.isNotEmpty()) baseDetailText_ += "\n\n";
        baseDetailText_ += "Why this helps: " + juce::String(rec_.why_helps);
    }
    personalReason_.clear();
    updateDetailText();
    renderable_ = hasRenderableChain(rec_);
    applyState_ = ApplyState::Idle;
    updateApplyButton();

    const bool hasEqParams = rec_.suggested_parameters.count("eq_freq_hz") > 0;
    tweakButton_.setEnabled(hasEqParams);

    if (hasEqParams) {
        // A finding may carry a frequency without a gain or Q (.at() threw and
        // took the Dashboard down); default them as the render does
        // (buildRecommendationChain: Q 0.707).
        const auto param = [this](const char* key, float fallback) {
            const auto it = rec_.suggested_parameters.find(key);
            return it != rec_.suggested_parameters.end() ? it->second : fallback;
        };
        freqSlider_.setRange(20.0, 20000.0, 1.0);
        freqSlider_.setValue(param("eq_freq_hz", 1000.0f), juce::dontSendNotification);

        gainSlider_.setRange(-12.0, 12.0, 0.1);
        gainSlider_.setValue(param("eq_gain_db", 0.0f), juce::dontSendNotification);

        qSlider_.setRange(0.1, 10.0, 0.01);
        qSlider_.setValue(param("eq_q", 0.707f), juce::dontSendNotification);
    }

    freqSlider_.setVisible(false);
    freqLabel_.setVisible(false);
    gainSlider_.setVisible(false);
    gainLabel_.setVisible(false);
    qSlider_.setVisible(false);
    qLabel_.setVisible(false);

    // Scan why_helps / engineer_note for SystemBrain encyclopedia entry matches
    juce::String advice = juce::String(rec_.why_helps) + " " + juce::String(rec_.engineer_note);
    advice = advice.toLowerCase();

    juce::String matchedId;
    juce::String matchedName;

#if !KHENSU_TESTS
    const auto features = controller_.analysis().systemBrainDatabase().search("");
    for (const auto& feat : features) {
        juce::String name = feat.name.c_str();
        if (advice.contains(name.toLowerCase())) {
            matchedId = feat.id.c_str();
            matchedName = name;
            break;
        }

        // Also check if any tags match as distinct words
        for (const auto& tag : feat.tags) {
            juce::String t = tag.c_str();
            if (t.length() > 3 && advice.contains(" " + t.toLowerCase() + " ")) {
                matchedId = feat.id.c_str();
                matchedName = name;
                break;
            }
        }
        if (matchedId.isNotEmpty()) break;
    }

    if (matchedId.isNotEmpty()) {
        learnMoreButton_.setButtonText(TRANS("Learn about ") + matchedName + " ->");
        learnMoreButton_.onClick = [this, matchedId]() {
            controller_.shell().selectSystemBrainFeature(matchedId.toStdString());
        };
    } else {
        learnMoreButton_.setButtonText("");
    }
#else
    juce::ignoreUnused(advice);
    learnMoreButton_.setButtonText("");
#endif

    if (onQueryFeedback) {
        auto res = onQueryFeedback(rec_.id);
        if (res.has_value()) {
            if (*res) {
                applyButton_.setAlpha(1.0f);
                dismissButton_.setAlpha(0.4f);
            } else {
                dismissButton_.setAlpha(1.0f);
                applyButton_.setAlpha(0.4f);
            }
        } else {
            applyButton_.setAlpha(1.0f);
            dismissButton_.setAlpha(1.0f);
        }
    }
    repaint();
}

juce::String RecommendationCard::evidenceSummaryFor(const Recommendation& rec)
{
    if (!rec.abstained_reason.empty())
        return "Caution - no confident action: " + juce::String(rec.abstained_reason);

    juce::String summary;
    if (!rec.diagnostic_evidence.empty())
        summary += "Evidence: " + juce::String(rec.diagnostic_evidence);

    if (rec.measurement_confidence > 0.0f) {
        if (summary.isNotEmpty()) summary += "\n";
        summary += "Measurement precision: "
            + juce::String(static_cast<int>(std::round(rec.measurement_confidence * 100.0f)))
            + "% (precision of the input measurement, not proof that this fix will help).";
    }

    if (!rec.confidence_reason.empty()) {
        if (summary.isNotEmpty()) summary += "\n";
        summary += "Measurement context: " + juce::String(rec.confidence_reason);
    }

    // CE-TRU-3: the measurements this card was decided on, and whether its
    // rule is calibrated against labelled data (CE-TRU-1).
    const auto lines = evidenceLinesFor(rec);
    if (lines.isNotEmpty()) {
        if (summary.isNotEmpty()) summary += "\n";
        summary += "Evidence:\n" + lines;
    }
    if (rec.type != RecommendationType::Strength) {
        if (summary.isNotEmpty()) summary += "\n";
        summary += "Confidence basis: " + FindingCalibrationCache::basisTag(issueCodeFor(rec)) + ".";
    }
    return summary;
}

juce::String RecommendationCard::issueCodeFor(const Recommendation& rec)
{
    const juce::String code(issueCodeOf(rec));
    if (code != juce::String(rec.id)) return code;
    // No source_file_id on a single-file result: the id is "<code>_<uuid>"
    // and uuids carry no underscore, so the last '_' is the boundary.
    const int sep = code.lastIndexOfChar('_');
    return sep > 0 ? code.substring(0, sep) : code;
}

juce::String RecommendationCard::evidenceLinesFor(const Recommendation& rec)
{
    auto num = [](float v, const std::string& unit) {
        const float a = std::abs(v);
        const int decimals = a >= 100.0f ? 0 : (a < 10.0f ? 2 : 1);
        juce::String s(v, decimals);
        if (!unit.empty()) s << " " << juce::String(unit);
        return s;
    };
    juce::StringArray out;
    for (const auto& e : rec.evidence) {
        juce::String line = "  " + juce::String(e.measurement) + ": " + num(e.value, e.unit);
        const juce::String source = e.source == "genre_norm"  ? "genre norm"
                                    : e.source == "reference" ? "reference track"
                                                              : "absolute standard";
        if (e.threshold.has_value()) {
            if ((e.comparison == "within" || e.comparison == "outside") && e.threshold_low.has_value())
                line << " (target " << num(*e.threshold_low, e.unit) << " to " << num(*e.threshold, e.unit) << ", "
                     << source << ")";
            else
                line << " (limit " << num(*e.threshold, e.unit) << ", " << source << ")";
        } else {
            line << " (" << source << ")";
        }
        out.add(line);
    }
    return out.joinIntoString("\n");
}

juce::Colour RecommendationCard::badgeColourForPriority(const juce::String& p)
{
    // Mix-note colour scheme (spec §4): red = severe, yellow = moderate,
    // blue = minor polish, green = something the mix does right.
    if (p.equalsIgnoreCase("priority")) return KhensuTheme::accentRed;
    if (p.equalsIgnoreCase("recommended")) return KhensuTheme::accentAmber;
    if (p.equalsIgnoreCase("strength")) return KhensuTheme::accentGreen;
    return KhensuTheme::accentBlue;
}

void RecommendationCard::mouseDown(const juce::MouseEvent& e)
{
    if (applyButton_.isVisible() &&
        (applyButton_.getBounds().contains(e.getPosition()) || dismissButton_.getBounds().contains(e.getPosition()) ||
         tweakButton_.getBounds().contains(e.getPosition()) || reasoningButton_.getBounds().contains(e.getPosition()) ||
         whatFixesButton_.getBounds().contains(e.getPosition()) ||
         freqSlider_.getBounds().contains(e.getPosition()) || gainSlider_.getBounds().contains(e.getPosition()) ||
         qSlider_.getBounds().contains(e.getPosition()))) {
        return;
    }
    expanded_ = !expanded_;
    updateApplyButton();
    applyButton_.setVisible(expanded_);
    dismissButton_.setVisible(expanded_);
    tweakButton_.setVisible(expanded_);
    reasoningButton_.setVisible(expanded_);
    whatFixesButton_.setVisible(expanded_ && rec_.type != RecommendationType::Strength);
    if (!expanded_) {
        tweaking_ = false;
        freqSlider_.setVisible(false);
        freqLabel_.setVisible(false);
        gainSlider_.setVisible(false);
        gainLabel_.setVisible(false);
        qSlider_.setVisible(false);
        qLabel_.setVisible(false);
    }
    if (auto* parent = getParentComponent()) parent->resized();
    repaint();
}

void RecommendationCard::paint(juce::Graphics& g)
{
    KhensuLookAndFeel::drawGlassPanel(g, getLocalBounds(), GlassPanelVariant::Default);

    auto b = getLocalBounds().reduced(KhensuTheme::sp3, 0);

    // Derive priority string from severity for badge colouring; strengths
    // (RecommendationType::Strength, severity 0) are green wins, not issues.
    juce::String priorityStr = rec_.type == RecommendationType::Strength ? "strength"
                               : rec_.severity >= 66.0f                  ? "priority"
                               : rec_.severity >= 33.0f                  ? "recommended"
                                                                         : "polish";
    juce::Colour badgeCol = badgeColourForPriority(priorityStr);

    // ── Collapsed row (always visible) ──────────────────────────────────
    auto row = b.removeFromTop(collapsedHeight());

    // Severity index number
    g.setFont(KhensuTheme::monoFont(type::caption));
    g.setColour(KhensuTheme::textTertiary);
    auto indexArea = row.removeFromLeft(28);
    g.drawText(juce::String(static_cast<int>(rec_.severity)), indexArea, juce::Justification::centredLeft, false);

    // Category pill - use RecommendationType as label text
    juce::String cat = juce::String(to_string(rec_.type)).toUpperCase();
    auto pillArea = row.removeFromLeft(80);
    g.setColour(badgeCol.withAlpha(0.2f));
    g.fillRoundedRectangle(pillArea.toFloat().reduced(0.0f, 8.0f), 4.0f);
    g.setColour(badgeCol);
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.drawText(cat, pillArea, juce::Justification::centred, false);

    row.removeFromLeft(KhensuTheme::sp2);

    // Action text - fall back to code extracted from id when suggested_action is empty.
    // id format: "<code>_<uuid-v4>" where uuid has hyphens, no underscores, so the
    // last '_' reliably marks the boundary between code and file_id.
    juce::String actionText(rec_.suggested_action);
    if (actionText.isEmpty()) {
        auto id = juce::String(rec_.id);
        auto sep = id.lastIndexOfChar('_');
        if (sep > 0) actionText = id.substring(0, sep).replace("_", " ");
        g.setFont(KhensuTheme::bodyFont(type::secondary));
        g.setColour(KhensuTheme::textTertiary);
    } else {
        g.setFont(KhensuTheme::bodyFont(type::secondary));
        g.setColour(KhensuTheme::textPrimary);
    }
    auto textArea = row.removeFromLeft(row.getWidth() - 20);
    if (rec_.type != RecommendationType::Strength && issueCode_.isNotEmpty()) {
        // CE-TRU-1: is this rule calibrated against labelled data? (tooltip)
        const auto tag = FindingCalibrationCache::basisTag(issueCode_);
        const bool calibrated = tag.startsWith("calibrated");
        g.setColour(calibrated ? KhensuTheme::accentGreen : KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.drawText(tag, textArea.removeFromRight(calibrated ? 104 : 64), juce::Justification::centredRight, false);
        g.setFont(KhensuTheme::bodyFont(type::secondary));
        g.setColour(rec_.suggested_action.empty() ? KhensuTheme::textTertiary : KhensuTheme::textPrimary);
    }
    if (personalReason_.isNotEmpty()) {
        // CE-AI-2: a small tag; the full reason leads the expanded detail.
        g.setColour(KhensuTheme::accentTeal);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.drawText(TRANS("FOR YOU"), textArea.removeFromRight(56), juce::Justification::centredRight, false);
        g.setFont(KhensuTheme::bodyFont(type::secondary));
        g.setColour(rec_.suggested_action.empty() ? KhensuTheme::textTertiary : KhensuTheme::textPrimary);
    }
    g.drawText(actionText, textArea, juce::Justification::centredLeft, true);

    // Expand arrow
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText(expanded_ ? juce::String(juce::CharPointer_UTF8("\xe2\x96\xbe"))
                         : juce::String(juce::CharPointer_UTF8("\xe2\x96\xb8")),
               row, juce::Justification::centred, false);

    // ── Expanded detail ──────────────────────────────────────────────────
    if (expanded_) {
        b.removeFromTop(KhensuTheme::sp2);

        // Separator rule
        g.setColour(KhensuTheme::borderSubtle);
        g.drawHorizontalLine(collapsedHeight(), static_cast<float>(KhensuTheme::sp3),
                             static_cast<float>(getWidth() - KhensuTheme::sp3));
        b.removeFromTop(2);

        // Pin chip row to bottom of expanded area; draw rationale in remaining space above
        auto expandedArea = b;

        // Suggested parameter chips - pinned to bottom
        if (!rec_.suggested_parameters.empty()) {
            auto chipRow = expandedArea.removeFromBottom(28);
            chipRow.removeFromRight(220); // reserve space for buttons on right
            for (const auto& [k, v] : rec_.suggested_parameters) {
                juce::String chip = juce::String(k) + ": " + juce::String(v, 1);
                const int chipW = juce::GlyphArrangement::getStringWidthInt(KhensuTheme::monoFont(type::caption), chip) + 16;
                if (chipRow.getWidth() < chipW) break;
                auto chipBounds = chipRow.removeFromLeft(chipW).reduced(0, 2);
                chipRow.removeFromLeft(4);
                g.setColour(KhensuTheme::bgOverlay);
                g.fillRoundedRectangle(chipBounds.toFloat(), 3.0f);
                g.setFont(KhensuTheme::monoFont(type::caption));
                g.setColour(KhensuTheme::textTertiary);
                g.drawText(chip, chipBounds, juce::Justification::centred, false);
            }
        }

        // Evidence, measurement precision/abstention, then rationale. The
        // numeric precision is intentionally never labelled as a likelihood
        // that the intervention works (Recommendation's contract).
        g.setFont(KhensuTheme::bodyFont(type::secondary));
        g.setColour(KhensuTheme::textSecondary);
        g.drawMultiLineText(detailText_, expandedArea.getX(), expandedArea.getY() + 12,
                            expandedArea.getWidth());
    }
}

void RecommendationCard::resized()
{
    const bool hasLink = !learnMoreButton_.getButtonText().isEmpty();
    learnMoreButton_.setVisible(expanded_ && hasLink);

    if (expanded_) {
        auto r = getLocalBounds().reduced(KhensuTheme::sp3, 0);
        r.removeFromTop(collapsedHeight() + KhensuTheme::sp2 + 2);

        // Reserve bottom area for action buttons
        auto chipRow = r.removeFromBottom(28);
        dismissButton_.setBounds(chipRow.removeFromRight(60).reduced(0, 2));
        chipRow.removeFromRight(6);
        tweakButton_.setBounds(chipRow.removeFromRight(60).reduced(0, 2));
        chipRow.removeFromRight(6);
        applyButton_.setBounds(chipRow.removeFromRight(60).reduced(0, 2));
        chipRow.removeFromRight(6);
        reasoningButton_.setBounds(chipRow.removeFromRight(76).reduced(0, 2));
        chipRow.removeFromRight(6);
        whatFixesButton_.setBounds(chipRow.removeFromRight(juce::jmin(140, juce::jmax(0, chipRow.getWidth() - 4)))
                                       .reduced(0, 2));

        if (hasLink) {
            learnMoreButton_.setBounds(chipRow.removeFromLeft(180).reduced(0, 4));
        }

        if (tweaking_) {
            // Layout inline sliders
            auto sliderArea = r.removeFromBottom(60).reduced(KhensuTheme::sp2, 2);
            int rowH = sliderArea.getHeight() / 3;

            auto fRow = sliderArea.removeFromTop(rowH);
            freqLabel_.setBounds(fRow.removeFromLeft(60));
            freqSlider_.setBounds(fRow);

            auto gRow = sliderArea.removeFromTop(rowH);
            gainLabel_.setBounds(gRow.removeFromLeft(60));
            gainSlider_.setBounds(gRow);

            auto qRow = sliderArea;
            qLabel_.setBounds(qRow.removeFromLeft(60));
            qSlider_.setBounds(qRow);
        }
    } else {
        applyButton_.setVisible(false);
        dismissButton_.setVisible(false);
        tweakButton_.setVisible(false);
        reasoningButton_.setVisible(false);
        whatFixesButton_.setVisible(false);

        freqSlider_.setVisible(false);
        freqLabel_.setVisible(false);
        gainSlider_.setVisible(false);
        gainLabel_.setVisible(false);
        qSlider_.setVisible(false);
        qLabel_.setVisible(false);
    }
}

} // namespace khensu
