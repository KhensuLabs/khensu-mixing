#pragma once
#include <JuceHeader.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <atomic>
#include <vector>

namespace khensu {

class KhensuCopilotProcessor;

/// Embedded VST3 chat console backed by copilotChat endpoint.
class ChatConsoleMiniPanel final : public juce::Component {
  public:
    explicit ChatConsoleMiniPanel(KhensuCopilotProcessor& processor);
    ~ChatConsoleMiniPanel() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /// The `history` array the next request will send: every COMPLETED
    /// exchange, oldest first. The pending message itself is sent as
    /// `message` and the service appends it (PLG-09), so it is never here.
    juce::var historyPayload() const;
    size_t historySize() const noexcept { return history_.size(); }

    /// Applies one finished request (also the test seam for PLG-09). Both
    /// turns are appended only when the reply arrived; a failed request leaves
    /// the history untouched instead of an orphan user turn.
    void completeExchange(const juce::String& userText, bool ok, const juce::String& replyOrError);

  private:
    void sendMessage();
    void appendTranscript(const juce::String& speaker, const juce::String& text, juce::Colour speakerColour);
    void setStatus(const juce::String& msg, bool isError = false);

    KhensuCopilotProcessor& processor_;

    juce::TextEditor transcript_;
    juce::TextEditor input_;
    juce::TextButton sendButton_{"Send"};
    juce::ToggleButton personalityToggle_{"Engineer Mode"}; // Unchecked = Producer Cat, Checked = Engineer Cat
    juce::Label statusLabel_;

    struct Turn {
        juce::String role, content;
    };
    std::vector<Turn> history_;

    std::atomic<bool> requestInFlight_{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChatConsoleMiniPanel)
};

} // namespace khensu
