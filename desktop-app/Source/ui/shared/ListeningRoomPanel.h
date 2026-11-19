// desktop-app/Source/ui/shared/ListeningRoomPanel.h
// CE-COL-3: start an opt-in LAN listening room for the A/B versions (the
// original, the current render and a few render-tree nodes), copy its link,
// and watch the blind votes arrive. The room itself is served by the local
// service on a separate LAN port (khensu_services/listening_room.py); it is
// off unless the master online switch and "LAN listening room" are both on.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h" // ThemeListener
#include "PanelCard.h"

namespace khensu {

class ListeningRoomPanel final : public PanelCard, private juce::Timer {
  public:
    explicit ListeningRoomPanel(IAppController& c);
    ~ListeningRoomPanel() override;

    void resized() override;
    void resetForSession() override;
    void visibilityChanged() override;

  private:
    void timerCallback() override;
    void startRoom();
    void stopRoom();
    void pollResults();
    void showResults(const juce::var& results);
    void setRunning(bool running);
    /// GET /listening/status: adopt a room this session already has open (the
    /// service keeps a room running when the panel is rebuilt or reshown).
    void syncWithService();

    juce::TextButton startButton_{TRANS("Start room")};
    juce::TextButton stopButton_{TRANS("Stop room")};
    juce::TextButton copyButton_{TRANS("Copy link")};
    juce::TextEditor urlField_;
    juce::Label statusLabel_;
    juce::Label votesLabel_;
    juce::String gainsText_; ///< loudness-match gain per version, from the start response
    bool running_ = false;
    bool pollInFlight_ = false;

    void applyThemeColours();
    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ListeningRoomPanel)
};

} // namespace khensu
