#pragma once
#include <JuceHeader.h>
#include <khensu/common/ThreadContext.h>
#include "KhensuComparisonStrip.h"
#include "MeterBridge.h"
#include "TransportWaveform.h"
#include "KhensuTheme.h" // ThemeListener

namespace khensu {

class IAppController;

class VectorIconButton final : public juce::Button {
  public:
    enum class IconType { Play, Pause, Stop };
    explicit VectorIconButton(IconType type) : juce::Button(""), type_(type) {}
    void setIconType(IconType type)
    {
        type_ = type;
        repaint();
    }
    void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override;

  private:
    IconType type_;
};

// THREAD_CONTEXT(UIThread)
class PlaybackTransportBar final : public juce::Component, private juce::Timer {
  public:
    /// Volume the slider starts at. AppController applies it from its own
    /// constructor body: this bar is built in AppController's initializer
    /// list, before most controller members exist, so its constructor must
    /// never call back into the controller.
    static constexpr float kDefaultVolume = 0.8f;

    explicit PlaybackTransportBar(IAppController& controller);
    ~PlaybackTransportBar() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    void showMonitorMenu();
    void timerCallback() override;
    void updateControls();

    IAppController& controller_;

    VectorIconButton playPauseButton_{VectorIconButton::IconType::Play};
    VectorIconButton stopButton_{VectorIconButton::IconType::Stop};
    /// UX2-5: the waveform scrubber (playhead, seek, the fix's changed regions).
    TransportWaveform waveform_{controller_};
    juce::Label timeLabel_;
    juce::Slider volumeSlider_;
    juce::Label volumeLabel_;
    KhensuComparisonStrip comparisonStrip_; // ritualized A/B/Δ/Blind control
    juce::TextButton undoButton_{TRANS("Undo")};   // undo applied render
    juce::TextButton redoButton_{TRANS("Redo")};   // redo applied render
    /// WF-1: what the next render processes - the current version (fixes
    /// stack) or the original. Shown once there is a version to start from.
    juce::ComboBox startFromBox_;
    /// UI-5: the compact meter rail - what is playing, measured live.
    MeterBridge meters_{controller_, MeterBridge::Layout::Compact};
    /// STU-5: Full / Mono / Side / Sub and Dim, on the device output only.
    juce::TextButton monitorButton_;
    juce::Rectangle<int> monitorBadgeArea_; ///< where the amber badge is painted
    juce::String monitorBadge_;             ///< its text ("" = not monitoring)


    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlaybackTransportBar)
};

} // namespace khensu
