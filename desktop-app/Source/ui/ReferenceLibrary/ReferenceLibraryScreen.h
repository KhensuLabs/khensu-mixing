#pragma once
#include <JuceHeader.h>
#include "../shared/ScreenHeader.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include "SimilarReferencesPanel.h"
#include "ReferenceProfilesBar.h"
#include <khensu/ipc/ILocalServiceBridge.h>
#include <vector>

namespace khensu {

class IAppController;

/// The "Reference Library" screen (FEAT-5): the user's own reference tracks -
/// listed, searched by similarity or description, set as the session's
/// reference - plus the genre fit: where the loaded mix sits against a
/// genre's reference samples, as numbers (the synthetic 3D cloud is gone,
/// CUT-6).
/// THREAD_CONTEXT(UIThread)
class ReferenceLibraryScreen final : public juce::Component, private juce::ListBoxModel {
  public:
    /// `baseUrl` points the panels at another service (tests: a fake one).
    explicit ReferenceLibraryScreen(IAppController& controller,
                                    const std::string& baseUrl = ipc::HttpServiceBridge::kDefaultBaseUrl);
    ~ReferenceLibraryScreen() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

  private:
    // juce::ListBoxModel
    int getNumRows() override;
    void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override;
    void selectedRowsChanged(int lastRowSelected) override;

    void refreshForSelectedGenre();
    void rebuildCloud(const juce::String& genreId, std::vector<khensu::ipc::GenreReferencePoint> points);
    void refreshStemEnergyLabel(const juce::String& genreId);

    /// Ranks all loaded genres by how closely their hand-tuned target
    /// (lufs_target, mean lra_range, spectral_tilt_target_db_per_octave,
    /// crest_factor_target_db -- the same fields GenreDatabase already
    /// derives/stores per genre, no new backend calls) matches the loaded
    /// track's own measured values. Purely a local Euclidean-distance
    /// comparison, not a claim about real reference-track proximity.
    void computeGenreRanking();

    IAppController& controller_;

    ScreenHeader titleLabel_; // UX-3: title = the sidebar label
    juce::Label percentileLabel_;
    juce::Label stemEnergyLabel_;
    juce::Label rankingLabel_;
    juce::ListBox genreList_;
    /// CE-AI-1: "Find similar" over the user's own indexed reference files.
    SimilarReferencesPanel similarPanel_;
    /// CE-COL-4: analyse / export / import / browse shared reference profiles.
    ReferenceProfilesBar profilesBar_;
    /// The user's own profiles for the shown genre, drawn as labelled points.
    std::vector<ReferenceProfilesBar::UserProfile> userProfiles_;
    /// The last reference sample shown, kept so a theme change can re-tint
    /// the cloud without refetching (UIX-20).
    std::vector<khensu::ipc::GenreReferencePoint> lastPoints_;
    juce::String lastPointsGenre_;

    juce::StringArray genreIds_;
    juce::StringArray genreNames_;
    int selectedRow_ = -1;

    // (genre id, distance) ascending -- index 0 is the closest match.
    std::vector<std::pair<juce::String, float>> genreRanking_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReferenceLibraryScreen)
};

} // namespace khensu
