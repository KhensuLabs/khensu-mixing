// desktop-app/Source/ui/ExportDelivery/ProjectBoardPanel.h
// FEAT-6: the project or album this track belongs to, as a readiness board -
// every track with its loudness, true peak, readiness score and which master
// the album uses (the accepted render, or the mix). Picks the track's project
// (or makes one), and opens another track's session with a click.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../../session/ProjectTracks.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/ListenerHandle.h"
#include "../shared/PanelCard.h"
#include <vector>

namespace khensu {

class IAppController;

class ProjectBoardPanel final : public PanelCard {
  public:
    explicit ProjectBoardPanel(IAppController& c);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void visibilityChanged() override;
    void resetForSession() override { refresh(); }

    /// Re-reads the projects and this track's project (off the message thread).
    void refresh();
    /// The rows shown (tests read it).
    const std::vector<project_tracks::Track>& tracks() const noexcept { return snapshot_.tracks; }

  private:
    void publish(project_tracks::Snapshot snap, const std::string& forSession);
    void rebuildProjectBox();
    void onProjectChosen();
    void promptNewProject();
    void promptRename();
    /// Runs a project change off the message thread, then tells every listener.
    void change(std::function<void(SessionManager&)> write);
    std::string activeSessionId() const;
    juce::Rectangle<int> listArea() const;
    int rowAt(juce::Point<int> p) const;

    static constexpr int kNoProjectId = 1;
    static constexpr int kFirstProjectId = 100;
    static constexpr int kRowH = 22;

    juce::Label projectLabel_;
    juce::ComboBox projectBox_;
    juce::TextButton newButton_{TRANS("New project...")};
    juce::TextButton renameButton_{TRANS("Rename...")};
    juce::Label statusLabel_;

    project_tracks::Snapshot snapshot_;
    int request_ = 0; ///< latest-wins
    int hoverRow_ = -1;
    std::unique_ptr<juce::AlertWindow> dialog_;

    void applyThemeColours();

    // After everything the callbacks touch.
    ListenerHandle projectsListener_;
    ListenerHandle analysisListener_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProjectBoardPanel)
};

} // namespace khensu
