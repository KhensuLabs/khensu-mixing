#include "PlaybackTransportBar.h"
#include "../../IAppController.h"
#include "KhensuLookAndFeel.h"

namespace khensu {

// ---------------------------------------------------------------------------
// VectorIconButton Implementation
// ---------------------------------------------------------------------------

void VectorIconButton::paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown)
{
    auto b = getLocalBounds().toFloat().reduced(1.0f);

    // Glassmorphic round button backplate
    g.setColour(isButtonDown ? KhensuTheme::bgVoid.withAlpha(0.8f)
                             : (isMouseOver ? KhensuTheme::bgSurface.brighter(0.08f).withAlpha(0.6f)
                                            : KhensuTheme::bgSurface.withAlpha(0.4f)));
    g.fillEllipse(b);

    // Glowing border highlight
    g.setColour(KhensuTheme::accentTeal.withAlpha(isMouseOver ? 0.7f : 0.3f));
    g.drawEllipse(b, 1.2f);

    // Center icon geometry
    auto iconArea = b.reduced(b.getWidth() * 0.30f);
    g.setColour(KhensuTheme::accentTeal);

    if (type_ == IconType::Play) {
        juce::Path p;
        p.addTriangle(iconArea.getX() + 1.5f, iconArea.getY(), iconArea.getX() + 1.5f, iconArea.getBottom(),
                      iconArea.getRight() + 1.0f, iconArea.getCentreY());
        g.fillPath(p);
    } else if (type_ == IconType::Pause) {
        float barW = iconArea.getWidth() * 0.28f;
        g.fillRect(iconArea.getX(), iconArea.getY(), barW, iconArea.getHeight());
        g.fillRect(iconArea.getRight() - barW, iconArea.getY(), barW, iconArea.getHeight());
    } else if (type_ == IconType::Stop) {
        g.fillRect(iconArea.reduced(1.0f));
    }
}

// ---------------------------------------------------------------------------
// PlaybackTransportBar Implementation
// ---------------------------------------------------------------------------

// Cached palette colours for the transport, in one place (UI-01). The bar is
// on screen on every screen, so a stale palette here is permanently visible.
void PlaybackTransportBar::applyThemeColours()
{
    timeLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    volumeLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    volumeSlider_.setColour(juce::Slider::trackColourId, KhensuTheme::bgVoid);
    volumeSlider_.setColour(juce::Slider::thumbColourId, KhensuTheme::textSecondary);
    undoButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgVoid);
    undoButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
    redoButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgVoid);
    redoButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
}

PlaybackTransportBar::PlaybackTransportBar(IAppController& controller) : controller_(controller)
{
    setFocusContainerType(juce::Component::FocusContainerType::focusContainer);
    applyThemeColours();
    playPauseButton_.setTitle(TRANS("Play or pause"));
    playPauseButton_.setDescription(TRANS("Start or pause audio playback."));
    playPauseButton_.setHelpText(TRANS("Press Space or Return to start or pause playback."));
    playPauseButton_.setExplicitFocusOrder(1);
    stopButton_.setTitle(TRANS("Stop"));
    stopButton_.setDescription(TRANS("Stop audio playback and return to the beginning."));
    stopButton_.setExplicitFocusOrder(2);
    // Wire Play/Pause Button
    playPauseButton_.onClick = [this]() {
        if (controller_.playback().isPlaying()) {
            controller_.playback().pauseAudio();
        } else {
            controller_.playback().playAudio();
        }
        updateControls();
    };
    addAndMakeVisible(playPauseButton_);

    // Wire Stop Button
    stopButton_.onClick = [this]() {
        controller_.playback().stopAudio();
        updateControls();
    };
    addAndMakeVisible(stopButton_);

    // UX2-5: the waveform scrubber replaces the bare position slider.
    waveform_.setExplicitFocusOrder(3);
    addAndMakeVisible(waveform_);

    // Time readout label
    timeLabel_.setText("00:00 / 00:00", juce::dontSendNotification);
    timeLabel_.setTitle(TRANS("Playback time"));
    timeLabel_.setDescription(TRANS("Elapsed playback time and total duration."));
    timeLabel_.setFont(khensu::KhensuTheme::numericFont(khensu::KhensuTheme::kFontBody)); // UI-4: tabular
    timeLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(timeLabel_);

    // Volume label
    volumeLabel_.setText(juce::CharPointer_UTF8("\xf0\x9f\x94\x8a"), juce::dontSendNotification); // Speaker icon
    volumeLabel_.setAccessible(false);
    volumeLabel_.setFont(khensu::KhensuTheme::uiFont(12.0f, juce::Font::plain));
    addAndMakeVisible(volumeLabel_);

    // Volume slider
    volumeSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    volumeSlider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    volumeSlider_.setRange(0.0, 1.0, 0.01);
    volumeSlider_.setValue(kDefaultVolume, juce::dontSendNotification);
    volumeSlider_.setTitle(TRANS("Playback volume"));
    volumeSlider_.setDescription(TRANS("Adjust the playback volume."));
    volumeSlider_.setHelpText(TRANS("Use Left and Right arrows to adjust volume."));
    volumeSlider_.setExplicitFocusOrder(4);
    volumeSlider_.onValueChange = [this]() {
        controller_.playback().setPlaybackVolume(static_cast<float>(volumeSlider_.getValue()));
    };
    // The initial level (kDefaultVolume) is applied by AppController's
    // constructor body, NOT here: this bar is constructed in AppController's
    // initializer list, so calling back into the controller now read members
    // that did not exist yet (an unconstructed std::string in
    // updatePlaybackGain) - an intermittent bad_alloc / heap-corruption crash
    // on launch.
    addAndMakeVisible(volumeSlider_);

    // Ritualized A/B comparison strip (kimi audit P1): replaces the small A/B
    // text buttons with Original/Processed/Δ/Blind + loudness-match indicator.
    comparisonStrip_.onAbToggled = [this](bool processed) {
        if (controller_.playback().isPlayingProcessed() != processed) controller_.playback().toggleAb();
        updateControls();
    };
    comparisonStrip_.onDeltaClicked = [this]() {
        if (controller_.playback().hasAbComparison()) controller_.renders().renderDeltaSolo({}, {});
    };
    comparisonStrip_.onAbxClicked = [this]() { controller_.shell().showAbxTrial(); };
    comparisonStrip_.onSnapshotSelected = [this](juce::String name) {
        // Switch the A/B "B" side to the selected render from history.
        for (int i = (int)controller_.renders().getAppliedRenderHistory().size() - 1; i >= 0; --i) {
            if (juce::File(controller_.renders().getAppliedRenderHistory()[(size_t)i]).getFileName() == name) {
                controller_.setAbProcessedPath(controller_.renders().getAppliedRenderHistory()[(size_t)i]);
                break;
            }
        }
    };
    comparisonStrip_.setComparisonAvailable(false);
    comparisonStrip_.setTitle(TRANS("A/B comparison"));
    comparisonStrip_.setDescription(TRANS("Switch between original, processed, delta, and blind audition modes."));
    comparisonStrip_.setExplicitFocusOrder(5);
    addAndMakeVisible(comparisonStrip_);

    // Undo/redo of applied copilot renders - audition history, not destructive
    // (each render is its own file on disk; these just switch what's loaded).
    undoButton_.setClickingTogglesState(false);
    undoButton_.setTitle(TRANS("Undo render"));
    undoButton_.setDescription(TRANS("Switch to the previous applied render."));
    undoButton_.setExplicitFocusOrder(6);
    undoButton_.onClick = [this]() {
        controller_.renders().undoRender();
        updateControls();
    };
    undoButton_.setVisible(false);
    addChildComponent(undoButton_);

    redoButton_.setClickingTogglesState(false);
    redoButton_.setTitle(TRANS("Redo render"));
    redoButton_.setDescription(TRANS("Switch to the next applied render."));
    redoButton_.setExplicitFocusOrder(7);
    redoButton_.onClick = [this]() {
        controller_.renders().redoRender();
        updateControls();
    };
    redoButton_.setVisible(false);
    addChildComponent(redoButton_);

    // WF-1: one app-wide "Start from" choice for every render (fix, EQ,
    // chain, mastering, chat). Read back in updateControls(), never set from
    // here: this bar is built before the controller's members exist.
    startFromBox_.addItem(TRANS("Start from current version"), 1);
    startFromBox_.addItem(TRANS("Start from original"), 2);
    startFromBox_.setSelectedId(1, juce::dontSendNotification);
    startFromBox_.setTitle(TRANS("Start renders from"));
    startFromBox_.setTooltip(TRANS("What the next fix or render processes: the current version, so fixes stack, "
                                   "or the original file."));
    startFromBox_.setExplicitFocusOrder(8);
    startFromBox_.onChange = [this] {
        controller_.renders().setRenderStart(startFromBox_.getSelectedId() == 2 ? IRenders::RenderStart::Original
                                                                                 : IRenders::RenderStart::CurrentVersion);
    };
    addChildComponent(startFromBox_);

    addChildComponent(meters_); // UI-5: shown with a session (updateControls)

    startTimer(40); // 25 fps updates
}

PlaybackTransportBar::~PlaybackTransportBar() { stopTimer(); }

void PlaybackTransportBar::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // Top border divider line
    g.setColour(KhensuTheme::borderActive);
    g.drawHorizontalLine(0, 0.0f, (float)bounds.getWidth());

    // Deep glassmorphic backplate
    g.setColour(KhensuTheme::bgBase.withAlpha(0.85f));
    g.fillRect(bounds.withTrimmedTop(1));
}

void PlaybackTransportBar::resized()
{
    auto b = getLocalBounds().reduced(8, 4);

    // Left controls: Play, Stop
    playPauseButton_.setBounds(b.removeFromLeft(36).withSizeKeepingCentre(28, 28));
    b.removeFromLeft(4);
    stopButton_.setBounds(b.removeFromLeft(36).withSizeKeepingCentre(28, 28));
    b.removeFromLeft(12);

    // Right controls: Start from, Redo, Undo, A/B comparison strip, then Volume slider
    if (b.getWidth() > 900) {
        startFromBox_.setBounds(b.removeFromRight(190).withSizeKeepingCentre(190, 26));
        b.removeFromRight(8);
    } else {
        startFromBox_.setBounds({});
    }
    // Wide enough for "Undo" / "Redo" at the 12 px floor (UX-4).
    redoButton_.setBounds(b.removeFromRight(52).withSizeKeepingCentre(52, 24));
    b.removeFromRight(2);
    undoButton_.setBounds(b.removeFromRight(52).withSizeKeepingCentre(52, 24));
    b.removeFromRight(8);
    // UI-6: A/B lives at the top of every session screen now; a second
    // strip here showed the same control twice. Kept (hidden) for the
    // listeners and the snapshot combo it still owns.
    comparisonStrip_.setBounds({});
    comparisonStrip_.setVisible(false);
    // UI-5: the compact meter rail.
    if (b.getWidth() > 700) {
        meters_.setBounds(b.removeFromRight(juce::jmin(400, b.getWidth() / 3)));
        b.removeFromRight(12);
    } else {
        meters_.setBounds({});
    }
    auto right = b.removeFromRight(150);
    volumeLabel_.setBounds(right.removeFromLeft(24).withSizeKeepingCentre(24, 24));
    volumeSlider_.setBounds(right.withSizeKeepingCentre(right.getWidth(), 16));

    b.removeFromRight(16);

    // Center time counter
    timeLabel_.setBounds(b.removeFromRight(100));
    b.removeFromRight(12);

    // The waveform scrubber takes the rest of the row.
    waveform_.setBounds(b.withSizeKeepingCentre(b.getWidth(), juce::jmin(b.getHeight(), 34)));
}

void PlaybackTransportBar::timerCallback()
{
    const double pos = controller_.playback().getPlaybackPosition();
    const double dur = controller_.playback().getPlaybackDuration();

    waveform_.update();

    auto formatTime = [](double seconds) -> juce::String {
        if (seconds < 0.0) seconds = 0.0;
        const int min = (int)(seconds / 60.0);
        const int sec = (int)std::fmod(seconds, 60.0);
        return juce::String(min).paddedLeft('0', 2) + ":" + juce::String(sec).paddedLeft('0', 2);
    };

    timeLabel_.setText(formatTime(pos) + " / " + formatTime(dur), juce::dontSendNotification);
    updateControls();
}

void PlaybackTransportBar::updateControls()
{
    if (controller_.playback().isPlaying()) {
        playPauseButton_.setIconType(VectorIconButton::IconType::Pause);
        playPauseButton_.setTitle(TRANS("Pause"));
        playPauseButton_.setDescription(TRANS("Pause audio playback."));
    } else {
        playPauseButton_.setIconType(VectorIconButton::IconType::Play);
        playPauseButton_.setTitle(TRANS("Play"));
        playPauseButton_.setDescription(TRANS("Start audio playback."));
    }

    // UX-5: A / B / Delta / Blind mean nothing without a track.
    comparisonStrip_.setVisible(false); // UI-6: A/B is at the top of the session screens
    if (meters_.isVisible() != controller_.hasActiveSession()) meters_.setVisible(controller_.hasActiveSession());
    const bool ab = controller_.playback().hasAbComparison();
    comparisonStrip_.setComparisonAvailable(ab);
    if (ab) {
        comparisonStrip_.setMode(controller_.playback().isPlayingProcessed());
        // Loudness-match status: true once the processed render's LUFS has been
        // measured (the underlying setAbProcessedPath path does this in the
        // background). We show "matched" whenever a real measurement exists,
        // because toggleAb gain-matches to that measurement.
        comparisonStrip_.setLoudnessMatched(controller_.playback().abProcessedLufs().has_value(), {});
        // Populate snapshot combo from the render-tree history.
        // Rebuilt only when the history changes; the selection follows the
        // ACTUAL B side (DSK-12).
        std::vector<juce::String> snaps;
        for (const auto& p : controller_.renders().getAppliedRenderHistory())
            snaps.push_back(juce::File(p).getFileName());
        const auto bPath = controller_.playback().abProcessedPath();
        comparisonStrip_.setSnapshots(snaps, bPath.isNotEmpty() ? juce::File(bPath).getFileName() : juce::String());
    }

    // Undo/redo - visible whenever there's any render history at all (i.e.
    // there's something to undo OR something already undone to redo to).
    const bool canUndo = controller_.renders().canUndoRender();
    const bool canRedo = controller_.renders().canRedoRender();
    const bool showHistoryControls = canUndo || canRedo;
    undoButton_.setVisible(showHistoryControls);
    redoButton_.setVisible(showHistoryControls);
    undoButton_.setEnabled(canUndo);
    redoButton_.setEnabled(canRedo);
    undoButton_.setTooltip(canUndo ? TRANS("Undo: revert to") + " " + controller_.renders().describeRenderAt(-1)
                                   : TRANS("Nothing to undo"));
    redoButton_.setTooltip(canRedo ? TRANS("Redo: switch to") + " " + controller_.renders().describeRenderAt(1)
                                   : TRANS("Nothing to redo"));

    // WF-1: the choice only matters once there is a version besides the
    // original (or the user already picked the original).
    const auto start = controller_.renders().renderStart();
    const bool showStartFrom = controller_.hasActiveSession() &&
                               (controller_.renders().currentRenderPath().isNotEmpty() ||
                                start == IRenders::RenderStart::Original);
    if (showStartFrom != startFromBox_.isVisible()) startFromBox_.setVisible(showStartFrom);
    const int wantId = start == IRenders::RenderStart::Original ? 2 : 1;
    if (startFromBox_.getSelectedId() != wantId) startFromBox_.setSelectedId(wantId, juce::dontSendNotification);
}

} // namespace khensu
