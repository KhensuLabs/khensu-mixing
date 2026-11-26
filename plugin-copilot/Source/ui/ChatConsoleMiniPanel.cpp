#include "ChatConsoleMiniPanel.h"
#include "PluginLookAndFeel.h"
#include "../PluginProcessor.h"
#include "../CopilotServiceJobs.h"
#include <khensu/ipc/generated/ApiSchema.h>
#include <khensu/ui/KhensuPalette.h>

namespace af = khensu::api::fields;

namespace khensu {

ChatConsoleMiniPanel::ChatConsoleMiniPanel(KhensuCopilotProcessor& processor) : processor_(processor)
{
    transcript_.setMultiLine(true);
    transcript_.setReadOnly(true);
    transcript_.setCaretVisible(false);
    transcript_.setScrollbarsShown(true);
    transcript_.setColour(juce::TextEditor::backgroundColourId, PluginLookAndFeel::bgPanel);
    transcript_.setColour(juce::TextEditor::outlineColourId, PluginLookAndFeel::controlOutline);
    transcript_.setColour(juce::TextEditor::textColourId, PluginLookAndFeel::textPrimary);
    addAndMakeVisible(transcript_);

    input_.setMultiLine(false);
    input_.setReturnKeyStartsNewLine(false);
    input_.setTextToShowWhenEmpty("Ask the copilot... e.g. \"make the vocals sound wet\" or \"clean up low-mid mud\"",
                                  PluginLookAndFeel::textMuted);
    input_.setColour(juce::TextEditor::backgroundColourId, PluginLookAndFeel::bgPanel);
    input_.setColour(juce::TextEditor::outlineColourId, PluginLookAndFeel::controlOutline);
    input_.setColour(juce::TextEditor::textColourId, PluginLookAndFeel::textPrimary);
    input_.onReturnKey = [this] { sendMessage(); };
    addAndMakeVisible(input_);

    sendButton_.setColour(juce::TextButton::buttonColourId, PluginLookAndFeel::accentTeal);
    sendButton_.setColour(juce::TextButton::textColourOffId, PluginLookAndFeel::bgDark);
    sendButton_.onClick = [this] { sendMessage(); };
    addAndMakeVisible(sendButton_);

    personalityToggle_.setButtonText("Engineer Mode");
    personalityToggle_.setColour(juce::ToggleButton::textColourId, PluginLookAndFeel::textMuted);
    personalityToggle_.setToggleState(true, juce::dontSendNotification); // Default: Engineer Mode
    addAndMakeVisible(personalityToggle_);

    statusLabel_.setFont(khensu::ui::Palette::uiFont(khensu::type::caption));
    statusLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::textMuted);
    addAndMakeVisible(statusLabel_);

    appendTranscript("Copilot", "Hi! Describe what you want to change or ask about your mix.", PluginLookAndFeel::accentTeal);
}

void ChatConsoleMiniPanel::paint(juce::Graphics& g) { g.fillAll(PluginLookAndFeel::bgDark); }

void ChatConsoleMiniPanel::resized()
{
    auto b = getLocalBounds().reduced(6);

    auto bottom = b.removeFromBottom(24);
    statusLabel_.setBounds(bottom.removeFromLeft(bottom.getWidth() / 2));
    personalityToggle_.setBounds(bottom);
    b.removeFromBottom(4);

    auto inputRow = b.removeFromBottom(24);
    sendButton_.setBounds(inputRow.removeFromRight(60));
    inputRow.removeFromRight(4);
    input_.setBounds(inputRow);
    b.removeFromBottom(4);

    transcript_.setBounds(b);
}

juce::var ChatConsoleMiniPanel::historyPayload() const
{
    juce::Array<juce::var> histVar;
    for (const auto& t : history_) {
        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty(af::ChatMessage::role, t.role);
        obj->setProperty(af::ChatMessage::content, t.content);
        histVar.add(juce::var(obj.get()));
    }
    return juce::var(histVar);
}

void ChatConsoleMiniPanel::completeExchange(const juce::String& userText, bool ok, const juce::String& replyOrError)
{
    requestInFlight_.store(false);
    if (!ok) {
        setStatus("Error: " + replyOrError, true);
        return;
    }
    setStatus("");
    appendTranscript("Copilot", replyOrError, PluginLookAndFeel::accentTeal);
    history_.push_back({"user", userText});
    history_.push_back({"assistant", replyOrError});
}

void ChatConsoleMiniPanel::sendMessage()
{
    juce::String text = input_.getText().trim();
    if (text.isEmpty() || requestInFlight_.load()) return;

    input_.clear();
    appendTranscript("You", text, PluginLookAndFeel::accentBlue);

    requestInFlight_.store(true);
    setStatus("Thinking...");

    // Only completed exchanges: the service appends `message` itself, so
    // adding the user turn here sent every message twice (PLG-09).
    const juce::var histVar = historyPayload();

    // Capture running meters for real-time context injection
    juce::DynamicObject::Ptr analysisObj = new juce::DynamicObject();
    analysisObj->setProperty("momentary_lufs", processor_.getMomentaryLufs());
    analysisObj->setProperty("short_term_lufs", processor_.getShortTermLufs());
    analysisObj->setProperty("correlation", processor_.getCorrelation());
    analysisObj->setProperty("stereo_width", processor_.getStereoWidth());
    analysisObj->setProperty("left_peak_db", processor_.getLeftPeakDb());
    analysisObj->setProperty("right_peak_db", processor_.getRightPeakDb());

    double bpm = processor_.getBpm();
    juce::String personality = personalityToggle_.getToggleState() ? "engineer" : "producer";

    juce::Component::SafePointer<ChatConsoleMiniPanel> self(this);

    processor_.runInBackground([self, msg = text, histVar, analysisVar = juce::var(analysisObj.get()), bpm,
                                personality]() {
        // Job + short polls instead of one blocking LLM round trip, so a
        // plugin unload (pool shouldExit) cancels it promptly (audit H3).
        const copilot::ShouldCancel shouldCancel = [] { return copilot::currentBackgroundJobShouldExit(); };
        auto res = copilot::copilotChatCancellable(msg, histVar, analysisVar, bpm, personality, shouldCancel);
        if (shouldCancel()) return; // unloading: no console left to update

        const bool ok = res.is_ok();
        const juce::String text = ok ? res.value().response : juce::String(res.error().message);
        juce::MessageManager::callAsync([self, msg, ok, text]() {
            if (self != nullptr) self->completeExchange(msg, ok, text);
        });
    });
}

void ChatConsoleMiniPanel::appendTranscript(const juce::String& speaker, const juce::String& text,
                                            juce::Colour speakerColour)
{
    transcript_.moveCaretToEnd();
    transcript_.setColour(juce::TextEditor::textColourId, speakerColour);
    transcript_.insertTextAtCaret("[" + speaker + "]: ");
    transcript_.setColour(juce::TextEditor::textColourId, PluginLookAndFeel::textPrimary);
    transcript_.insertTextAtCaret(text + "\n\n");
    transcript_.moveCaretToEnd();
}

void ChatConsoleMiniPanel::setStatus(const juce::String& msg, bool isError)
{
    statusLabel_.setText(msg, juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId,
                           isError ? PluginLookAndFeel::statusBad : PluginLookAndFeel::textMuted);
}

} // namespace khensu
