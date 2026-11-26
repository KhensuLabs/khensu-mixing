// desktop-app/Source/ui/shared/LivePreviewToggle.h
// STU-9: "Preview live" for any surface that edits a chain before it is
// rendered. The surface gives a chain provider and calls chainChanged() on
// every edit; while the toggle is on, the transport plays its output through
// that chain (IPlayback::setLivePreviewChain - native stages, block-wise,
// loudness matched). A note says which stages the preview cannot run live.
//
// The preview never writes a file or registers a version: the surface's own
// Keep / Apply still renders through renders().renderVersion() and the fix
// check - and should call setPreviewing(false) when it does, so the new
// version is not heard through the chain twice. The preview also stops when
// the surface leaves the screen and on a session change.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <vector>
#include <khensu/ipc/HttpServiceBridge.h>
#include "KhensuTheme.h" // ThemeListener

namespace khensu {

class IAppController;

class LivePreviewToggle final : public juce::Component, private juce::Timer {
  public:
    using ChainProvider = std::function<std::vector<ipc::HttpServiceBridge::EffectSpec>()>;

    /// `label` names the preview ("EQ curve"). Stores the controller only:
    /// safe to build in a screen's initializer list (ENG-2).
    LivePreviewToggle(IAppController& controller, juce::String label, ChainProvider provider);
    ~LivePreviewToggle() override;

    /// The surface's chain changed: sent again while previewing.
    void chainChanged();
    void setPreviewing(bool on);
    bool isPreviewing() const noexcept { return toggle_.getToggleState(); }

    void resized() override;

  private:
    void timerCallback() override;
    void push();
    void refreshNote();
    void applyThemeColours();

    IAppController& controller_;
    juce::String label_;
    ChainProvider provider_;
    juce::ToggleButton toggle_;
    juce::Label note_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LivePreviewToggle)
};

} // namespace khensu
