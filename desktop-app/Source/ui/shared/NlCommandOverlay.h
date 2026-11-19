// desktop-app/Source/ui/shared/NlCommandOverlay.h
#pragma once
#include <JuceHeader.h>
#include "ExperimentalBadge.h"
#include "KhensuTheme.h" // ThemeListener
#include "ModalCardHost.h"
#include "PushToTalk.h"
#include "VoiceRecorder.h"
#include "../../IAppController.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <atomic>
#include <memory>
#include <string>

namespace khensu {

/// Free-text mix instruction from the command palette (CE-AG-4):
/// "make the chorus 1 dB louder than the verse" -> /command/parse -> a
/// preview of exactly what will be rendered (effects, which sections, which
/// stem, what could not be done) -> Confirm -> /command/execute (a job through
/// the guarded render pipeline) -> the result joins the render history.
/// Nothing renders before Confirm.
///
/// Push-to-talk: hold the mic button (or tap it, or press it from the
/// keyboard, to start and again to stop), speak the instruction, release.
/// The recording goes to a temp WAV, /command/voice transcribes and parses it
/// off the message thread, and the transcript + plan land in the same
/// preview. An empty `text` opens the overlay ready for voice.
// THREAD_CONTEXT(UIThread)
class NlCommandOverlay final : public ModalCardHost, private juce::Timer {
  public:
    NlCommandOverlay(IAppController& controller, const juce::String& text,
                     std::string baseUrl = ipc::HttpServiceBridge::kDefaultBaseUrl);
    ~NlCommandOverlay() override;

    void resized() override;

    std::function<void()> onDismiss;

    /// The preview text for a /command/parse response (CommandParseResponse
    /// JSON). Static + public so it is unit-testable without a service.
    static juce::String formatPreview(const juce::var& parsed);

  protected:
    juce::Rectangle<int> cardBounds() const override { return card_.getBounds(); }
    void dismiss() override
    {
        if (onDismiss) onDismiss();
    }

  private:
    void startParse();
    void sessionInputs(juce::String& filePath, std::map<juce::String, juce::String>& stems);
    void handlePtt(push_to_talk::Machine::Action action);
    void startRecording();
    void stopAndSend();
    void onVoiceResult(const juce::var& parsed);
    void timerCallback() override;

    /// Mic button reporting press / release (mouse) and keyboard or
    /// accessibility activation separately.
    class MicButton final : public juce::TextButton {
      public:
        MicButton() : juce::TextButton(TRANS("Hold to talk")) {}
        std::function<void()> onPress, onRelease, onActivate;
        void mouseDown(const juce::MouseEvent& e) override
        {
            juce::TextButton::mouseDown(e);
            if (onPress) onPress();
        }
        void mouseUp(const juce::MouseEvent& e) override
        {
            inMouseUp_ = true;
            juce::TextButton::mouseUp(e);
            inMouseUp_ = false;
            if (onRelease) onRelease();
        }

      protected:
        using juce::TextButton::clicked;
        void clicked() override
        {
            if (!inMouseUp_ && onActivate) onActivate(); // keyboard / screen reader
        }

      private:
        bool inMouseUp_ = false;
    };
    void onParsed(const juce::var& parsed);
    void confirm();
    void onExecuted(bool ok, const juce::String& message, const juce::String& outputPath);

    IAppController& controller_;
    std::string baseUrl_;
    juce::String text_;
    juce::String inputPath_; ///< WF-1: what the command processed
    juce::String sessionId_;
    juce::String planId_;

    juce::Component card_;
    juce::Label titleLabel_;
    ExperimentalBadge experimentalBadge_; ///< VER-8
    juce::Label instructionLabel_;
    juce::Label statusLabel_;
    juce::TextEditor preview_;
    juce::TextButton confirmButton_{TRANS("Render it")};
    juce::TextButton closeButton_{TRANS("Close")};
    MicButton micButton_;
    VoiceRecorder recorder_;
    push_to_talk::Machine ptt_;
    std::shared_ptr<std::atomic<bool>> stop_ = std::make_shared<std::atomic<bool>>(false);

    void applyThemeColours();
    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NlCommandOverlay)
};

} // namespace khensu
