#pragma once
#include <JuceHeader.h>
#include "../shared/ScreenHeader.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include "GenreFitChart.h"
#include "ReferenceScanPanel.h"
#include "SimilarReferencesPanel.h"
#include "ReferenceProfilesBar.h"
#include <khensu/ipc/ILocalServiceBridge.h>
#include <vector>

namespace khensu {

class IAppController;

/// The "Reference Library" screen (FEAT-5, REFS-1, UXR-4): a genre picker;
/// the songs, samples and packs the reference folder scan sorted into that
/// genre; the user's own reference library (find similar, set as the
/// session's reference); and on the right the genre's norms - where the
/// loaded mix sits against its reference samples, its stem-energy targets
/// and a chart of the closest genre fits.
/// THREAD_CONTEXT(UIThread)
class ReferenceLibraryScreen final : public juce::Component {
  public:
    /// `baseUrl` points the panels at another service (tests: a fake one).
    explicit ReferenceLibraryScreen(IAppController& controller,
                                    const std::string& baseUrl = ipc::HttpServiceBridge::kDefaultBaseUrl);
    ~ReferenceLibraryScreen() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

    /// What a picker entry selects (exposed for tests).
    struct PickerChoice {
        juce::String genre;  ///< catalog genre id
        juce::String family; ///< family id ("All Hip-Hop")
        bool unsorted = false;
    };
    /// The genre the screen shows: the catalog genre picked, or a family
    /// that is itself a catalog genre; empty for All / Unsorted / other families.
    juce::String selectedGenreId() const;
    /// Picks a catalog genre (or family id) in the picker, as a user would.
    void selectGenre(const juce::String& id);
    /// The picker's choices by combo id - 1 (exposed for tests).
    const std::vector<PickerChoice>& pickerChoices() const noexcept { return choices_; }

  private:
    void rebuildPicker();
    void pickerChanged();
    void refreshForSelectedGenre();
    void rebuildCloud(const juce::String& genreId, std::vector<khensu::ipc::GenreReferencePoint> points);
    void refreshStemEnergyLabel(const juce::String& genreId);

    /// Ranks all loaded genres by how closely their targets (lufs_target,
    /// mean lra_range, spectral_tilt_target_db_per_octave,
    /// crest_factor_target_db - fields GenreDatabase already stores per
    /// genre, no backend call) match the loaded track's measured values, and
    /// shows the ranking in `fitChart_`. A local normalised-distance
    /// comparison, not a claim about real reference-track proximity.
    void computeGenreRanking();

    IAppController& controller_;

    ScreenHeader titleLabel_; // UX-3: title = the sidebar label
    juce::Label genreCaption_;
    juce::ComboBox genrePicker_;
    juce::Label genreHint_;
    juce::Label percentileLabel_;
    juce::Label stemEnergyLabel_;
    GenreFitChart fitChart_;
    /// REFS-1: the scanned songs / samples / packs of the picked genre.
    ReferenceScanPanel scanPanel_;
    /// CE-AI-1: "Find similar" over the user's own indexed reference files.
    SimilarReferencesPanel similarPanel_;
    /// CE-COL-4: analyse / export / import / browse shared reference profiles.
    ReferenceProfilesBar profilesBar_;
    /// The user's own profiles for the shown genre.
    std::vector<ReferenceProfilesBar::UserProfile> userProfiles_;
    /// The last reference sample shown, kept so a theme change can redraw
    /// without refetching (UIX-20).
    std::vector<khensu::ipc::GenreReferencePoint> lastPoints_;
    juce::String lastPointsGenre_;

    std::vector<PickerChoice> choices_;
    juce::var counts_; ///< last scan summary: per-genre / family counts
    int selectedId_ = 1;
    bool pickerInitialised_ = false;

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
