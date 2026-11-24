// desktop-app/Source/ui/Copilot/FeedbackDecoderPanel.cpp
#include "../shared/ServiceErrorHint.h"
#include "FeedbackDecoderPanel.h"
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace af = khensu::api::fields;

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void FeedbackDecoderPanel::applyThemeColours()
{
    titleLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    decodeButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    decodeButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

FeedbackDecoderPanel::FeedbackDecoderPanel(IAppController& c) : controller_(c)
{
    applyThemeColours();
    titleLabel_.setText(TRANS("CLIENT REVISION DECODER"), juce::dontSendNotification);
    titleLabel_.setFont(khensu::KhensuTheme::uiFont(16.0f, juce::Font::bold));
    addAndMakeVisible(titleLabel_);

    feedbackInput_.setMultiLine(true, true);
    feedbackInput_.setReturnKeyStartsNewLine(true);
    feedbackInput_.setTextToShowWhenEmpty(
        TRANS("Paste the client's note...  e.g. \"love it, but can it be warmer and the vocals feel a bit buried\""),
        KhensuTheme::textTertiary);
    feedbackInput_.setFont(KhensuTheme::bodyFont(13.0f));
    addAndMakeVisible(feedbackInput_);

    decodeButton_.onClick = [this] { decode(); };
    addAndMakeVisible(decodeButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(10.0f));
    statusLabel_.setText(TRANS("Clients describe feelings; this turns them into moves."), juce::dontSendNotification);
    addAndMakeVisible(statusLabel_);

    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);
}

void FeedbackDecoderPanel::decode()
{
    const juce::String text = feedbackInput_.getText().trim();
    if (text.isEmpty()) {
        statusLabel_.setText(TRANS("Paste some client feedback first."), juce::dontSendNotification);
        return;
    }
    statusLabel_.setText(TRANS("Decoding..."), juce::dontSendNotification);
    decodeButton_.setEnabled(false);

    juce::Component::SafePointer<FeedbackDecoderPanel> self(this);
    controller_.runInBackground([self, text]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.decodeClientFeedback(text.toStdString());
        juce::MessageManager::callAsync([self, result]() {
            if (self == nullptr) return;
            self->decodeButton_.setEnabled(true);
            if (result.is_err()) {
                self->statusLabel_.setText(TRANS("Decode failed: ") + serviceErrorLine(result.error()),
                                           juce::dontSendNotification);
                return;
            }
            const juce::var v = result.value();
            self->cards_.clear();

            int count = 0;
            if (auto* matches = v[af::DecodeFeedbackResponse::matches].getArray()) {
                for (const auto& m : *matches) {
                    const juce::String concept = m[af::FeedbackMatch::concept].toString();
                    if (auto* interps = m[af::FeedbackMatch::interpretations].getArray()) {
                        for (const auto& iv : *interps) {
                            auto card = std::make_unique<Card>();
                            card->data.concept = concept;
                            card->data.reading = iv[af::FeedbackInterpretation::reading].toString();
                            card->data.explanation = iv[af::FeedbackInterpretation::explanation].toString();
                            card->data.confidence = (float)(double)iv[af::FeedbackInterpretation::confidence];
                            if (auto* obj = iv[af::FeedbackInterpretation::parameters].getDynamicObject())
                                for (const auto& prop : obj->getProperties())
                                    card->data.parameters[prop.name.toString()] = (float)(double)prop.value;
                            auto* cardPtr = card.get();
                            card->applyButton.onClick = [self, cardPtr] {
                                if (self != nullptr) self->applyInterpretation(cardPtr->data);
                            };
                            self->content_.addAndMakeVisible(*card);
                            self->cards_.push_back(std::move(card));
                            ++count;
                        }
                    }
                }
            }
            const bool unmatched = (bool)v[af::DecodeFeedbackResponse::unmatched];
            self->statusLabel_.setText(unmatched
                                           ? "No known concepts matched - try the Chat tab for free-form feedback."
                                           : juce::String(count) + " interpretation(s) - audition before you commit.",
                                       juce::dontSendNotification);
            self->resized();
            self->repaint();
        });
    });
}

void FeedbackDecoderPanel::applyInterpretation(const Interpretation& interp)
{
    Recommendation rec;
    rec.id = "client_feedback_" + juce::Uuid().toString().toStdString();
    rec.what = ("Client asked: " + interp.concept + " - " + interp.reading).toStdString();
    rec.most_likely_cause = interp.explanation.toStdString();
    rec.confidence = interp.confidence;
    for (const auto& [k, val] : interp.parameters)
        rec.suggested_parameters[k.toStdString()] = val;

    statusLabel_.setText(TRANS("Rendering \"") + interp.reading + "\"...", juce::dontSendNotification);
    juce::Component::SafePointer<FeedbackDecoderPanel> self(this);
    controller_.renders().applyRecommendationChain(rec, [self](bool ok, juce::String msg) {
        if (self == nullptr) return;
        self->statusLabel_.setText(ok ? "Rendered - use the transport A/B to audition." : "Apply failed: " + msg,
                                   juce::dontSendNotification);
    });
}

void FeedbackDecoderPanel::Card::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().reduced(2);
    g.setColour(KhensuTheme::bgRaised);
    g.fillRoundedRectangle(b.toFloat(), 6.0f);
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.25f));
    g.drawRoundedRectangle(b.toFloat(), 6.0f, 1.0f);

    auto inner = b.reduced(10, 6);
    auto top = inner.removeFromTop(16);
    g.setFont(KhensuTheme::bodyFont(12.0f).boldened());
    g.setColour(KhensuTheme::textPrimary);
    g.drawText(data.reading, top.removeFromLeft(top.getWidth() - 140), juce::Justification::centredLeft, true);

    g.setFont(KhensuTheme::labelFont(9.0f).boldened());
    g.setColour(KhensuTheme::accentTeal);
    g.drawText(data.concept.toUpperCase() + "  " + juce::String((int)(data.confidence * 100)) + "%", top,
               juce::Justification::centredRight, false);

    inner.removeFromBottom(26); // apply button row
    g.setFont(KhensuTheme::labelFont(10.0f));
    g.setColour(KhensuTheme::textSecondary);
    g.drawFittedText(data.explanation, inner, juce::Justification::topLeft, 3);
}

void FeedbackDecoderPanel::Card::resized()
{
    auto b = getLocalBounds().reduced(12, 8);
    applyButton.setBounds(b.removeFromBottom(22).removeFromLeft(110));
}

void FeedbackDecoderPanel::paint(juce::Graphics& g) { g.fillAll(KhensuTheme::bgBase); }

void FeedbackDecoderPanel::resized()
{
    auto b = getLocalBounds().reduced(16);
    titleLabel_.setBounds(b.removeFromTop(26));
    b.removeFromTop(4);
    feedbackInput_.setBounds(b.removeFromTop(72));
    b.removeFromTop(6);
    auto row = b.removeFromTop(26);
    decodeButton_.setBounds(row.removeFromLeft(150));
    row.removeFromLeft(8);
    statusLabel_.setBounds(row);
    b.removeFromTop(8);
    viewport_.setBounds(b);

    const int cardH = 108;
    content_.setSize(juce::jmax(50, viewport_.getWidth() - 10), juce::jmax(1, (int)cards_.size() * (cardH + 6)));
    int y = 0;
    for (auto& cardComp : cards_) {
        cardComp->setBounds(0, y, content_.getWidth(), cardH);
        y += cardH + 6;
    }
}

} // namespace khensu
