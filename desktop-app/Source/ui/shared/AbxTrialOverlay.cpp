// desktop-app/Source/ui/shared/AbxTrialOverlay.cpp
#include "AbxTrialOverlay.h"
#include "KhensuLookAndFeel.h"
#include "../../IAppController.h"
#include "KhensuTheme.h"
#include "AbxStatistics.h"

namespace khensu {

// Every cached palette colour on this overlay, in one place (UI-01).
// preferenceLabel_/trialStatusLabel_ are additionally tinted by the trial
// result paths; those re-resolve on the next trial update.
void AbxTrialOverlay::applyThemeColours()
{
    cardColour_ = KhensuTheme::bgSurface;
    titleLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    trialStatusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    scoreLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    deltaStatusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    preferenceLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    deltaSoloButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    deltaSoloButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    startTrialButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    startTrialButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
}

AbxTrialOverlay::AbxTrialOverlay(IAppController& controller, juce::StringArray blindReferencePaths)
    : controller_(controller), blindReferencePaths_(std::move(blindReferencePaths)),
      savedProcessedPath_(controller.playback().abProcessedPath()), savedShowingProcessed_(controller.playback().isPlayingProcessed())
{
    backdropAlpha_ = 0.75f;
    applyThemeColours();
    borderColour_ = KhensuTheme::borderSubtle;
    borderThickness_ = 1.5f;
    cornerRadius_ = 6.0f;

    addAndMakeVisible(card_);

    titleLabel_.setText(TRANS("Delta Solo & Blind ABX"), juce::dontSendNotification);
    titleLabel_.setFont(khensu::KhensuTheme::uiFont(15.0f, juce::Font::bold));
    card_.addAndMakeVisible(titleLabel_);

    closeButton_.onClick = [this]() {
        if (onDismiss) onDismiss();
    };
    card_.addAndMakeVisible(closeButton_);

    deltaSoloButton_.setTooltip(TRANS("Play only what the processing chain added/removed "
                                "(processed minus original) - kills placebo listening."));
    deltaSoloButton_.onClick = [this]() { playDeltaSolo(); };
    card_.addAndMakeVisible(deltaSoloButton_);

    deltaStatusLabel_.setFont(khensu::KhensuTheme::uiFont(10.5f, juce::Font::plain));
    card_.addAndMakeVisible(deltaStatusLabel_);

    startTrialButton_.onClick = [this]() { startTrial(); };
    card_.addAndMakeVisible(startTrialButton_);

    trialStatusLabel_.setFont(khensu::KhensuTheme::uiFont(11.0f, juce::Font::plain));
    // Honest labelling (§10.3): against a reference this is an
    // IDENTIFICATION test (can you tell your mix from another master?), not a
    // transparency test of your processing.
    const auto setupText =
        isBlindReferenceMode()
            ? TRANS("Identification test: press Start to hear your mix or a hidden reference, then say which played.")
            : TRANS("Press Start to hear a randomly chosen A or B, then guess which one played.");
    trialStatusLabel_.setText(setupText, juce::dontSendNotification);
    card_.addAndMakeVisible(trialStatusLabel_);

    guessAButton_.setEnabled(false);
    guessAButton_.onClick = [this]() { guess(false); };
    card_.addAndMakeVisible(guessAButton_);

    guessBButton_.setEnabled(false);
    guessBButton_.onClick = [this]() { guess(true); };
    card_.addAndMakeVisible(guessBButton_);

    scoreLabel_.setFont(khensu::KhensuTheme::uiFont(12.0f, juce::Font::bold));
    scoreLabel_.setText(TRANS("Score: 0 / 0"), juce::dontSendNotification);
    card_.addAndMakeVisible(scoreLabel_);

    preferenceLabel_.setFont(khensu::KhensuTheme::uiFont(10.5f, juce::Font::plain));
    preferenceLabel_.setText(TRANS("Separately, tell the app which you prefer at equal loudness - it learns "
                                   "whether you like the processing, not a loudness."),
                             juce::dontSendNotification);
    card_.addAndMakeVisible(preferenceLabel_);

    preferAButton_.onClick = [this] { recordPreference(false); };
    card_.addAndMakeVisible(preferAButton_);
    preferBButton_.onClick = [this] { recordPreference(true); };
    card_.addAndMakeVisible(preferBButton_);
    if (isBlindReferenceMode()) {
        // DSK-07: B is someone else's master here - a preference for it says
        // nothing about the user's processing, so it is not offered.
        preferAButton_.setEnabled(false);
        preferBButton_.setEnabled(false);
        preferenceLabel_.setText(TRANS("Preference learning is off for reference comparisons - B is another "
                                       "master, not your processing."),
                                 juce::dontSendNotification);

        // DSK-08: B is muted until its loudness is known. Measure every
        // reference first so the first trial never plays silence exactly
        // when the answer is B.
        referencesMeasured_ = false;
        startTrialButton_.setEnabled(false);
        startTrialButton_.setButtonText(TRANS("Measuring references..."));
        juce::Component::SafePointer<AbxTrialOverlay> self(this);
        controller_.playback().primeAbLoudness(blindReferencePaths_, [self](bool) {
            if (self == nullptr) return;
            self->referencesMeasured_ = true;
            self->startTrialButton_.setEnabled(true);
            self->startTrialButton_.setButtonText(TRANS("Start Blind Trial"));
        });
    }

    grabModalFocus();
}

AbxTrialOverlay::~AbxTrialOverlay() = default;

void AbxTrialOverlay::restorePlaybackState()
{
    if (!touchedPlayback_) return;
    touchedPlayback_ = false;
    deltaActive_ = false;
    if (bSlotBorrowed_) {
        bSlotBorrowed_ = false;
        controller_.setAbProcessedPath(savedProcessedPath_);
    }
    controller_.playback().setAbSide(savedShowingProcessed_);
}

void AbxTrialOverlay::playDeltaSolo()
{
    if (deltaActive_) {
        // Toggle off: return to whatever normal A/B playback was showing.
        deltaActive_ = false;
        deltaSoloButton_.setButtonText(TRANS("Play Delta (Null Test)"));
        deltaStatusLabel_.setText({}, juce::dontSendNotification);
        // Reload whichever side A/B was on (toggleAb() here used to FLIP it).
        controller_.playback().setAbSide(controller_.playback().isPlayingProcessed());
        return;
    }

    deltaSoloButton_.setEnabled(false);
    deltaStatusLabel_.setText(TRANS("Rendering null test..."), juce::dontSendNotification);

    juce::Component::SafePointer<AbxTrialOverlay> self(this);
    controller_.renders().renderDeltaSolo({}, [self](bool ok, juce::String pathOrError) {
        if (self == nullptr) return;
        self->deltaSoloButton_.setEnabled(true);
        if (!ok) {
            self->deltaStatusLabel_.setText(TRANS("Failed:") + " " + pathOrError, juce::dontSendNotification);
            return;
        }
        self->deltaActive_ = true;
        self->touchedPlayback_ = true;
        self->deltaSoloButton_.setButtonText(TRANS("Stop Delta Solo"));
        self->deltaStatusLabel_.setText(TRANS("Playing only the change the chain made."), juce::dontSendNotification);
    });
}

void AbxTrialOverlay::startTrial()
{
    if (!referencesMeasured_) return; // DSK-08: Start is held until then
    if (blindReferencePaths_.isEmpty() && !controller_.playback().hasAbComparison()) {
        trialStatusLabel_.setText(TRANS("No A/B comparison loaded yet."), juce::dontSendNotification);
        return;
    }

    activeBlindReferenceName_.clear();
    if (const auto referencePath = chooseBlindReference(); referencePath.isNotEmpty()) {
        // Always return to A before replacing B. Otherwise an already-playing
        // B source would continue playing after the path changes.
        if (controller_.playback().isPlayingProcessed()) controller_.playback().setAbSide(false);
        controller_.setAbProcessedPath(referencePath);
        bSlotBorrowed_ = true; // restored on dismiss (AUDIT-001)
        activeBlindReferenceName_ = juce::File(referencePath).getFileNameWithoutExtension();
    }

    actualIsProcessed_ = juce::Random::getSystemRandom().nextBool();
    touchedPlayback_ = true;
    if (deltaActive_) { // the trial replaces the delta preview
        deltaActive_ = false;
        deltaSoloButton_.setButtonText(TRANS("Play Delta (Null Test)"));
        deltaStatusLabel_.setText({}, juce::dontSendNotification);
    }
    controller_.playback().setAbSide(actualIsProcessed_);
    controller_.playback().playAudio();

    trialInProgress_ = true;
    guessAButton_.setEnabled(true);
    guessBButton_.setEnabled(true);
    startTrialButton_.setButtonText(TRANS("Restart Trial"));
    trialStatusLabel_.setText(TRANS("Playing... which one do you think this is?"), juce::dontSendNotification);
}

void AbxTrialOverlay::guess(bool guessedProcessed)
{
    if (!trialInProgress_) return;

    ++totalCount_;
    const bool correct = (guessedProcessed == actualIsProcessed_);
    if (correct) ++correctCount_;

    trialInProgress_ = false;
    guessAButton_.setEnabled(false);
    guessBButton_.setEnabled(false);

    const juce::String processedLabel = activeBlindReferenceName_.isNotEmpty()
                                            ? TRANS("reference") + " \"" + activeBlindReferenceName_ + "\" (B)."
                                            : TRANS("the processed render (B).");
    trialStatusLabel_.setText((correct ? TRANS("Correct!") : TRANS("Not quite.")) + " " + TRANS("That was actually") +
                                  " " + (actualIsProcessed_ ? processedLabel : TRANS("the original (A).")),
                              juce::dontSendNotification);
    trialStatusLabel_.setColour(juce::Label::textColourId,
                                correct ? KhensuTheme::accentTeal : KhensuTheme::accentAmber);

    // §10.3: a binomial p-value, not a bare hit rate.
    scoreLabel_.setText(abx_stats::describeScore(correctCount_, totalCount_), juce::dontSendNotification);
}

juce::String AbxTrialOverlay::chooseBlindReference()
{
    if (blindReferencePaths_.isEmpty()) return {};

    int index = juce::Random::getSystemRandom().nextInt(blindReferencePaths_.size());
    if (blindReferencePaths_.size() > 1 && index == lastBlindReferenceIndex_)
        index = (index + 1) % blindReferencePaths_.size();
    lastBlindReferenceIndex_ = index;
    return blindReferencePaths_[index];
}

void AbxTrialOverlay::recordPreference(bool preferredProcessed)
{
    if (isBlindReferenceMode()) return; // DSK-07: nothing to learn from another master
    controller_.playback().learnFromAbPreference(preferredProcessed, /*bIsBlindReference*/ false);
    preferenceLabel_.setText((preferredProcessed ? TRANS("Noted: at equal loudness you preferred the processed (B) render.")
                                                 : TRANS("Noted: at equal loudness you preferred the original (A).")),
                             juce::dontSendNotification);
    preferenceLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
}

void AbxTrialOverlay::resized()
{
    auto bounds = getLocalBounds();
    auto cardW = juce::jmin(480, bounds.getWidth() - 40);
    auto cardH = juce::jmin(460, bounds.getHeight() - 40);
    card_.setBounds((bounds.getWidth() - cardW) / 2, (bounds.getHeight() - cardH) / 2, cardW, cardH);

    auto b = card_.getLocalBounds().reduced(24);
    auto header = b.removeFromTop(32);
    closeButton_.setBounds(header.removeFromRight(90));
    titleLabel_.setBounds(header);
    b.removeFromTop(16);

    deltaSoloButton_.setBounds(b.removeFromTop(30));
    b.removeFromTop(4);
    deltaStatusLabel_.setBounds(b.removeFromTop(18));
    b.removeFromTop(20);

    auto sep = b.removeFromTop(1);
    juce::ignoreUnused(sep);
    b.removeFromTop(8);

    startTrialButton_.setBounds(b.removeFromTop(30));
    b.removeFromTop(8);
    trialStatusLabel_.setBounds(b.removeFromTop(34));
    b.removeFromTop(8);

    auto guessRow = b.removeFromTop(30);
    guessAButton_.setBounds(guessRow.removeFromLeft(guessRow.getWidth() / 2 - 4));
    guessRow.removeFromLeft(8);
    guessBButton_.setBounds(guessRow);
    b.removeFromTop(12);

    scoreLabel_.setBounds(b.removeFromTop(24));
    b.removeFromTop(16);

    preferenceLabel_.setBounds(b.removeFromTop(30));
    b.removeFromTop(6);
    auto preferRow = b.removeFromTop(30);
    preferAButton_.setBounds(preferRow.removeFromLeft(preferRow.getWidth() / 2 - 4));
    preferRow.removeFromLeft(8);
    preferBButton_.setBounds(preferRow);
}

} // namespace khensu
