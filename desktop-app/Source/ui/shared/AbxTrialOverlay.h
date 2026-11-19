// desktop-app/Source/ui/shared/AbxTrialOverlay.h
#pragma once
#include <JuceHeader.h>
#include "ModalCardHost.h"
#include "KhensuTheme.h" // ThemeListener

namespace khensu {

class IAppController;

/// Trust-building UI: delta solo (null-test audition of only what a chain
/// changed) plus a blind ABX trial that scores whether the user can
/// genuinely hear the A/B difference, rather than trusting a sighted
/// "does it look/measure better" comparison. Requires an existing A/B
/// comparison (AppController::hasAbComparison()).
class AbxTrialOverlay final : public ModalCardHost {
  public:
    explicit AbxTrialOverlay(IAppController& controller, juce::StringArray blindReferencePaths = {});
    ~AbxTrialOverlay() override;

    void resized() override;

    std::function<void()> onDismiss;

    /// Puts back the A/B "B" path and the A/B side that were active when the
    /// overlay opened (a blind trial borrows the B slot for its references,
    /// the delta solo replaces the loaded file). Called by
    /// AppController::hideAbxTrial() before the overlay is destroyed - a
    /// reference left in the B slot used to become the export (AUDIT-001).
    void restorePlaybackState();


  protected:
    juce::Rectangle<int> cardBounds() const override { return card_.getBounds(); }
    void dismiss() override
    {
        if (onDismiss) onDismiss();
    }

  private:
    void playDeltaSolo();
    void startTrial();
    void guess(bool guessedProcessed);
    juce::String chooseBlindReference();

    IAppController& controller_;

    juce::Component card_;
    juce::Label titleLabel_;
    juce::TextButton closeButton_{TRANS("Close")};

    juce::TextButton deltaSoloButton_{TRANS("Play Delta (Null Test)")};
    juce::Label deltaStatusLabel_;
    bool deltaActive_ = false;
    juce::String restoreStemPath_; // path playing before delta solo, to restore on toggle-off

    juce::TextButton startTrialButton_{TRANS("Start Blind Trial")};
    juce::TextButton guessAButton_{TRANS("I hear: Original (A)")};
    juce::TextButton guessBButton_{TRANS("I hear: Processed (B)")};
    juce::Label trialStatusLabel_;
    juce::Label scoreLabel_;

    // Not blind - an explicit "which do you actually like better" pick,
    // separate from the ABX audibility test, that feeds the taste profile
    // (AppController::learnFromAbPreference).
    juce::Label preferenceLabel_;
    juce::TextButton preferAButton_{TRANS("I Prefer Original (A)")};
    juce::TextButton preferBButton_{TRANS("I Prefer Processed (B)")};
    void recordPreference(bool preferredProcessed);

    bool trialInProgress_ = false;
    bool actualIsProcessed_ = false;
    int correctCount_ = 0;
    int totalCount_ = 0;
    juce::StringArray blindReferencePaths_;
    juce::String activeBlindReferenceName_;
    int lastBlindReferenceIndex_ = -1;

    // A/B state on open, restored by restorePlaybackState().
    juce::String savedProcessedPath_;
    bool savedShowingProcessed_ = false;
    bool bSlotBorrowed_ = false;   ///< a blind reference replaced the B path
    bool touchedPlayback_ = false; ///< a trial or delta solo changed what is loaded
    bool referencesMeasured_ = true; ///< DSK-08: Start waits for the references' loudness
    bool isBlindReferenceMode() const noexcept { return !blindReferencePaths_.isEmpty(); }

    /// Re-applies every cached palette colour on this overlay. Called at
    /// construction and on each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AbxTrialOverlay)
};

} // namespace khensu
