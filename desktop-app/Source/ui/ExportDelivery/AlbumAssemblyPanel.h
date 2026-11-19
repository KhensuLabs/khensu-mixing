// desktop-app/Source/ui/ExportDelivery/AlbumAssemblyPanel.h
// Album Assembly: cross-track consistency for an EP/album - per-track
// LUFS / tonal tilt / crest with seam flags (loudness jumps, tonal
// shifts, density mismatches). The album is the product, not the song.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../../session/ProjectTracks.h"
#include <vector>

namespace khensu {

class IAppController;

class AlbumAssemblyPanel final : public PanelCard {
  public:
    explicit AlbumAssemblyPanel(IAppController& c);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;

  private:
    /// Moves the selected track one place up (-1) or down (+1) and
    /// re-measures the seams, which depend on the order (UIX-21: the order
    /// used to be whatever the OS file chooser returned).
    void moveSelected(int delta);
    juce::Rectangle<int> matrixArea() const;
    int selectedTrack_ = -1;
    int analysisRequest_ = 0; ///< latest-wins: a reorder supersedes an in-flight measurement
    void chooseTracks();
    /// The file chooser (tracks not tied to sessions).
    void browseTracks();
    /// FEAT-6: the project's tracks - each session's current master, in the
    /// project's order; a reorder here is saved as the project's order.
    void useProject(const project_tracks::Snapshot& snap);
    void runAnalysis();
    void runNormalizationSim(); ///< track-mode vs album-mode loudness jumps

    struct Track {
        juce::String name;
        float lufs = 0.0f, crest = 0.0f, tilt = 0.0f;
    };

    juce::TextButton chooseButton_{TRANS("Add Album Tracks...")};
    juce::TextButton normSimButton_{TRANS("Normalization sim")};
    juce::TextButton masterButton_{TRANS("Master album...")}; ///< opens AlbumMasterDialog
    juce::TextButton upButton_{TRANS("Move up")};
    juce::TextButton downButton_{TRANS("Move down")};
    juce::Label statusLabel_;
    juce::Label flagsLabel_;

    juce::StringArray trackPaths_;
    std::vector<std::string> trackSessionIds_; ///< parallel to trackPaths_ when loaded from a project
    std::string projectId_;                   ///< the project they came from ("" = chosen files)
    std::vector<Track> tracks_;
    juce::String verdict_;

    std::unique_ptr<juce::FileChooser> chooser_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AlbumAssemblyPanel)
};

} // namespace khensu
