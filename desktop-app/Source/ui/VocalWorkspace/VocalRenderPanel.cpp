// desktop-app/Source/ui/VocalWorkspace/VocalRenderPanel.cpp
#include "VocalRenderPanel.h"
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/RenderPaths.h"
#include <khensu/common/EventHub.h>
#include "../../RenderJobManager.h"

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void VocalRenderPanel::applyThemeColours()
{
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

VocalRenderPanel::VocalRenderPanel(IAppController& c) : PanelCard(c, "Vocal DSP Render")
{
    applyThemeColours();
    setExplainer(TRANS("Vocal DSP Render"),
                 TRANS("Renders the 7 Vocal Macro sliders above onto the isolated vocal stem so you can actually "
                 "hear them, not just read the labels. Auto-render fires ~600ms after you stop dragging a "
                 "slider. Switch to Manual to batch several changes before rendering. Download exports the "
                 "last render as a standalone WAV."));
    setAnalysisState(PanelAnalysisState::NotApplicable);

    autoRenderToggle_.setToggleState(true, juce::dontSendNotification);
    autoRenderToggle_.onClick = [this] {
        autoMode_ = autoRenderToggle_.getToggleState();
        renderNowButton_.setVisible(!autoMode_);
        if (autoMode_ && pendingChangeCount_ > 0) startRender();
        setStatusText(autoMode_ ? TRANS("Up to date") : TRANS("Manual mode"));
    };
    addAndMakeVisible(autoRenderToggle_);

    statusLabel_.setFont(khensu::KhensuTheme::uiFont(11.0f, juce::Font::plain));
    addAndMakeVisible(statusLabel_);

    renderNowButton_.setVisible(false);
    renderNowButton_.onClick = [this] { startRender(); };
    addChildComponent(renderNowButton_);

    playPreviewButton_.setEnabled(false);
    playPreviewButton_.setTooltip(TRANS("Play the processed vocal on its own; click again to go back to the mix."));
    playPreviewButton_.onClick = [this] { togglePreview(); };
    addAndMakeVisible(playPreviewButton_);

    downloadButton_.setEnabled(false);
    downloadButton_.onClick = [this] { chooseDownloadLocation(); };
    addAndMakeVisible(downloadButton_);

    EventHub::getInstance().addListener(this, EventType::VocalMacroChanged,
                                        [this](const EventData&) { onMacroValueChanged(); });

    setStatusText(TRANS("Waiting for stems"));
    refreshEnabledState();
}

VocalRenderPanel::~VocalRenderPanel()
{
    EventHub::getInstance().removeListener(this);
    stopTimer();
}

void VocalRenderPanel::refreshEnabledState()
{
    stemsAvailable_ = controller_.stems().hasStemsAvailable();
    autoRenderToggle_.setEnabled(stemsAvailable_);
    renderNowButton_.setEnabled(stemsAvailable_);
    if (!stemsAvailable_) {
        // No stems (e.g. a session without them): nothing previous may leak in.
        previewing_ = false;
        lastOutputPath_ = {};
        playPreviewButton_.setButtonText(TRANS("Play Preview"));
        playPreviewButton_.setEnabled(false);
        downloadButton_.setEnabled(false);
        setStatusText(TRANS("Requires separated stems (run \"Isolate Stems\" above)"));
    } else if (lastOutputPath_.isEmpty()) {
        setStatusText(TRANS("Up to date -- adjust a macro slider to render"));
    }
}

void VocalRenderPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto row1 = b.removeFromTop(24);
    autoRenderToggle_.setBounds(row1.removeFromLeft(120));
    renderNowButton_.setBounds(row1.removeFromLeft(100));
    b.removeFromTop(4);
    statusLabel_.setBounds(b.removeFromTop(18));
    b.removeFromTop(6);
    auto row2 = b.removeFromTop(26);
    playPreviewButton_.setBounds(row2.removeFromLeft(row2.getWidth() / 2 - 4));
    row2.removeFromLeft(8);
    downloadButton_.setBounds(row2);
}

void VocalRenderPanel::setStatusText(const juce::String& text, bool isError)
{
    statusLabel_.setText(text, juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId, isError ? khensu::KhensuTheme::accentRed : KhensuTheme::textSecondary);
}

void VocalRenderPanel::onMacroValueChanged()
{
    if (!stemsAvailable_) return;
    ++pendingChangeCount_;
    if (autoMode_) {
        startTimer(kDebounceMs); // resets the countdown on every call
        setStatusText(TRANS("Rendering in %ss...").replace("%s", juce::String(kDebounceMs / 1000.0, 1)));
    } else {
        setStatusText(TRANS("Stale -- %n change(s) pending").replace("%n", juce::String(pendingChangeCount_)));
    }
}

void VocalRenderPanel::timerCallback()
{
    stopTimer();
    startRender();
}

void VocalRenderPanel::startRender()
{
    if (!stemsAvailable_) return;
    stopTimer();
    pendingChangeCount_ = 0;

    // Supersede: cancel any of this panel's own render jobs still in flight
    // by scanning RenderJobManager::activeJobs() for a matching description
    // (a chain render is void and has no way to report a
    // job's id back to the caller synchronously -- the id is only known
    // internally, assigned asynchronously after the /start call completes --
    // so activeJobs() is the only way to find and cancel our own prior job).
    // The generation counter below is still the correctness guarantee for
    // stale *callback results*; this cancel is a best-effort backend-compute
    // saver on top of that. cancelJob() is keyed by localId, which is valid
    // immediately, so this now also cancels a job whose jobId hasn't been
    // assigned yet instead of silently letting it render unattended.
    for (const auto& job : controller_.renderJobs().activeJobs())
        if (job.description == "Vocal macro preview") controller_.renderJobs().cancelJob(job.localId);
    const int generation = ++renderGeneration_;
    renderInFlight_ = true;

    mixerchain::VocalMacroValues v;
    v.compression = (int)controller_.vocalEdit().getVocalMacroValue("Compression");
    v.presence = (int)controller_.vocalEdit().getVocalMacroValue("Presence");
    v.width = (int)controller_.vocalEdit().getVocalMacroValue("Width");
    v.deEssing = (int)controller_.vocalEdit().getVocalMacroValue("De-essing");
    v.saturation = (int)controller_.vocalEdit().getVocalMacroValue("Saturation");
    v.reverb = (int)controller_.vocalEdit().getVocalMacroValue("Reverb");
    v.delay = (int)controller_.vocalEdit().getVocalMacroValue("Delay");
    const auto chain = mixerchain::buildVocalMacroChain(v);

    // The macro preview runs on the current vocal (WF-4): the edited one
    // when an edit stacked, else the separated stem.
    const juce::String edited = controller_.stems().currentVocalStem();
    const juce::String inPath = edited.isNotEmpty() ? edited : controller_.getStemFile("vocals").getFullPathName();
    // A fresh file per render: rewriting one fixed "vocal_macro_preview.wav"
    // while the transport had it open as B played torn / stale audio with a
    // stale loudness match (AUDIT-018). The superseded preview is deleted
    // once the new one is loaded (onRenderFinished).
    const juce::String outPath =
        uniqueRenderFile(controller_.stems().getStemsDirectory().getChildFile("vocal_macro_preview.wav"), {})
            .getFullPathName();
    setStatusText(TRANS("Rendering..."));

    if (chain.empty()) {
        // All macros at 0: nothing to render -- fall back to the raw stem so
        // "Play Preview"/"Download" still work with an honest passthrough.
        renderInFlight_ = false;
        lastOutputPath_ = inPath;
        onRenderFinished(generation, true, inPath, {});
        return;
    }

    juce::Component::SafePointer<VocalRenderPanel> self(this);
    IRenders::ChainRender render;
    render.description = "Vocal macro preview";
    render.chain = chain;
    controller_.renders().renderPreview(
        render, inPath, outPath,
        [self, generation](IRenders::RenderOutcome outcome, const juce::String& pathOrError,
                           const juce::var& fullResult) {
            if (self == nullptr) return;
            if (outcome == IRenders::RenderOutcome::SessionChanged) { // AUDIT-002
                // Reset the busy state anyway, or "Rendering..." sticks (UIX-17).
                if (generation == self->renderGeneration_) {
                    self->renderInFlight_ = false;
                    self->setStatusText(TRANS("Session changed - the preview was discarded."));
                }
                return;
            }
            self->onRenderFinished(generation, outcome == IRenders::RenderOutcome::Rendered,
                                   outcome == IRenders::RenderOutcome::Failed ? pathOrError : juce::String(),
                                   fullResult);
        });
}

void VocalRenderPanel::onRenderFinished(int generation, bool ok, const juce::String& pathOrError, const juce::var&)
{
    if (generation != renderGeneration_) return; // superseded by a newer render
    renderInFlight_ = false;

    if (!ok) {
        if (pendingChangeCount_ > 0) return; // a newer render is about to start anyway
        setStatusText(pathOrError.isNotEmpty() ? (TRANS("Render failed: ") + pathOrError) : TRANS("Render cancelled"),
                      pathOrError.isNotEmpty());
        return;
    }

    const juce::String previous = lastOutputPath_;
    lastOutputPath_ = pathOrError;
    downloadButton_.setEnabled(true);
    playPreviewButton_.setEnabled(true);
    applyProcessedPathSeamless(lastOutputPath_);
    // The superseded preview is now unreferenced (the transport was just
    // moved off it, and previews are never registered as renders): delete it
    // so per-render names don't pile up. Never the raw stem (the all-zero
    // passthrough).
    const juce::File old(previous);
    if (previous.isNotEmpty() && previous != lastOutputPath_ && old.getFileName().startsWith("vocal_macro_preview"))
        old.deleteFile();
    setStatusText(TRANS("Up to date"));
}

void VocalRenderPanel::applyProcessedPathSeamless(const juce::String& path)
{
    // The preview is a lone processed vocal. It used to become the mix A/B
    // "B" side, so the A/B compared the full mix with a solo vocal (UIX-18).
    // Now it is only ever loaded straight into the transport - and only while
    // the user is previewing - swapped in at the current position so a
    // re-render while listening is seamless.
    if (!previewing_) return;
    const double position = controller_.playback().getPlaybackPosition();
    const bool wasPlaying = controller_.playback().isPlaying();
    controller_.playback().loadAudioFileForPlayback(juce::File(path));
    controller_.playback().setPlaybackPosition(position);
    if (wasPlaying) controller_.playback().playAudio();
}

void VocalRenderPanel::togglePreview()
{
    if (!previewing_) {
        if (lastOutputPath_.isEmpty() || !juce::File(lastOutputPath_).existsAsFile()) return;
        previewing_ = true;
        applyProcessedPathSeamless(lastOutputPath_);
        if (!controller_.playback().isPlaying()) controller_.playback().playAudio();
        playPreviewButton_.setButtonText(TRANS("Back to Mix"));
        return;
    }
    previewing_ = false;
    // Reloads the mix (the A side), keeping the position and play state.
    controller_.playback().setAbSide(false);
    playPreviewButton_.setButtonText(TRANS("Play Preview"));
}

void VocalRenderPanel::chooseDownloadLocation()
{
    if (lastOutputPath_.isEmpty() || !juce::File(lastOutputPath_).existsAsFile()) return;

    downloadChooser_ = std::make_unique<juce::FileChooser>(
        TRANS("Save Processed Vocal As..."),
        juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile("vocal_processed.wav"), "*.wav");

    const auto flags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                       juce::FileBrowserComponent::warnAboutOverwriting;

    juce::Component::SafePointer<VocalRenderPanel> self(this);
    const juce::String sourcePath = lastOutputPath_;
    downloadChooser_->launchAsync(flags, [self, sourcePath](const juce::FileChooser& fc) {
        if (self == nullptr) return;
        auto dest = fc.getResult();
        if (dest == juce::File{}) return;

        if (!juce::File(sourcePath).copyFileTo(dest))
            self->setStatusText(TRANS("Download failed -- could not write %f").replace("%f", dest.getFullPathName()), true);
        else
            self->setStatusText(TRANS("Saved to %f").replace("%f", dest.getFileName()));
    });
}

} // namespace khensu
