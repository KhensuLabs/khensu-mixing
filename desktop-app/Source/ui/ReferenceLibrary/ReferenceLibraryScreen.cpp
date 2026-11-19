#include "ReferenceLibraryScreen.h"
#include "../shared/KhensuTheme.h"
#include "../../IAppController.h"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <khensu/targets/GenreDatabase.h>

namespace khensu {


// Every cached palette colour on this component, in one place (UI-01).
void ReferenceLibraryScreen::applyThemeColours()
{
    percentileLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    stemEnergyLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    rankingLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    genreList_.setColour(juce::ListBox::backgroundColourId, KhensuTheme::bgBase);
    // The cloud's point colours come from the theme: rebuild them from the
    // cached sample so a theme switch reaches the cloud too (UIX-20).
    if (!lastPoints_.empty()) rebuildCloud(lastPointsGenre_, lastPoints_);
}

ReferenceLibraryScreen::ReferenceLibraryScreen(IAppController& controller, const std::string& baseUrl)
    : controller_(controller), similarPanel_(controller, baseUrl),
      profilesBar_(controller, [this] {
          return (selectedRow_ >= 0 && selectedRow_ < genreIds_.size()) ? genreIds_[selectedRow_] : juce::String();
      }, baseUrl)
{
    applyThemeColours();
    addAndMakeVisible(similarPanel_);
    addAndMakeVisible(profilesBar_);
    profilesBar_.onProfilesChanged = [this](const juce::String& genreId,
                                            std::vector<ReferenceProfilesBar::UserProfile> profiles) {
        userProfiles_ = std::move(profiles);
        if (genreId == lastPointsGenre_ && !lastPoints_.empty()) rebuildCloud(lastPointsGenre_, lastPoints_);
    };
    titleLabel_.setScreen(ScreenType::ReferenceLibrary);
    addAndMakeVisible(titleLabel_);

    percentileLabel_.setFont(KhensuTheme::bodyFont(12.0f));
    percentileLabel_.setJustificationType(juce::Justification::topLeft);
    percentileLabel_.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(percentileLabel_);

    stemEnergyLabel_.setFont(KhensuTheme::bodyFont(11.0f));
    stemEnergyLabel_.setJustificationType(juce::Justification::topLeft);
    stemEnergyLabel_.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(stemEnergyLabel_);

    rankingLabel_.setFont(KhensuTheme::bodyFont(11.5f));
    rankingLabel_.setJustificationType(juce::Justification::topLeft);
    rankingLabel_.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(rankingLabel_);

    for (const auto& g : controller_.genreDatabase().listAll()) {
        genreIds_.add(g.id);
        genreNames_.add(g.name);
    }

    genreList_.setModel(this);
    addAndMakeVisible(genreList_);

}

ReferenceLibraryScreen::~ReferenceLibraryScreen() { genreList_.setModel(nullptr); }

void ReferenceLibraryScreen::visibilityChanged()
{
    if (!isVisible()) return;
    similarPanel_.showLibrary(); // FEAT-5: the user's references, listed on open
    computeGenreRanking();
    if (selectedRow_ >= 0) return;

    // Default the selection to the loaded song's own (detected or chosen)
    // genre, if any, so opening this screen for the first time on a loaded
    // session shows something relevant instead of an arbitrary first row.
    juce::String defaultGenreId;
    if (controller_.hasAnalysis() && controller_.currentAnalysis().benchmark)
        defaultGenreId = juce::String(controller_.currentAnalysis().benchmark->genre_id);
    else if (auto* session = controller_.getActiveSession())
        defaultGenreId = juce::String(session->target.primary_genre);

    int row = genreIds_.indexOf(defaultGenreId);
    if (row < 0) row = 0;
    if (genreIds_.isEmpty()) return;

    // selectRow notifies selectedRowsChanged, which fetches; fetch here only
    // when it did not (the row was already selected), so the first show asks
    // the service once, not twice (found by VER-6).
    genreList_.selectRow(row);
    if (selectedRow_ != row) {
        selectedRow_ = row;
        refreshForSelectedGenre();
    }
}

void ReferenceLibraryScreen::computeGenreRanking()
{
    genreRanking_.clear();
    if (!controller_.hasAnalysis()) {
        rankingLabel_.setText(TRANS("Load and analyze a track to see a comparative genre ranking."),
                              juce::dontSendNotification);
        return;
    }

    const auto& analysis = controller_.currentAnalysis();
    const float songLufs = analysis.loudness.lufs_integrated;
    const float songLra = analysis.loudness.lra;
    const float songTilt = analysis.spectral.tilt_db_per_octave;
    const float songCrest = analysis.dynamics.crest_factor_db;

    // A plain normalized-Euclidean-distance fit score against each genre's
    // own hand-tuned target curve (GenreDatabase already derives/stores
    // these fields per genre -- no extra backend calls needed). This is a
    // "how close does this mix sit to that genre's typical numbers" ranking,
    // not a claim about matching real reference tracks.
    for (const auto& g : controller_.genreDatabase().listAll()) {
        const float genreLra = (g.lra_range_min + g.lra_range_max) * 0.5f;
        const float dLufs = (songLufs - g.lufs_target) / 3.0f;
        const float dLra = (songLra - genreLra) / 4.0f;
        const float dTilt = (songTilt - g.spectral_tilt_target_db_per_octave) / 3.0f;
        const float dCrest = (songCrest - g.crest_factor_target_db) / 4.0f;
        const float dist = std::sqrt(dLufs * dLufs + dLra * dLra + dTilt * dTilt + dCrest * dCrest);
        genreRanking_.push_back({juce::String(g.id), dist});
    }
    std::sort(genreRanking_.begin(), genreRanking_.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    juce::String text = "COMPARATIVE RANKING\nClosest genre fit to this mix:\n\n";
    for (int i = 0; i < juce::jmin(10, (int)genreRanking_.size()); ++i) {
        auto entry = controller_.genreDatabase().findById(genreRanking_[(size_t)i].first.toStdString());
        const juce::String name = entry ? juce::String(entry->name) : genreRanking_[(size_t)i].first;
        text << (i + 1) << ". " << name << juce::String(juce::CharPointer_UTF8("  (fit \xce\x94")) << juce::String(genreRanking_[(size_t)i].second, 2)
             << ")\n";
    }
    rankingLabel_.setText(text, juce::dontSendNotification);
}

void ReferenceLibraryScreen::paint(juce::Graphics& g) { g.fillAll(KhensuTheme::bgBase); }

void ReferenceLibraryScreen::resized()
{
    auto b = getLocalBounds().reduced(12);
    titleLabel_.setBounds(b.removeFromTop(28));
    b.removeFromTop(6);
    profilesBar_.setBounds(b.removeFromTop(44)); // CE-COL-4
    b.removeFromTop(4);

    genreList_.setBounds(b.removeFromLeft(180));
    b.removeFromLeft(10);

    auto right = b.removeFromRight(260);
    b.removeFromRight(10);
    percentileLabel_.setBounds(right.removeFromTop(right.getHeight() * 40 / 100));
    right.removeFromTop(8);
    stemEnergyLabel_.setBounds(right.removeFromTop(right.getHeight() * 30 / 100));
    right.removeFromTop(8);
    rankingLabel_.setBounds(right);

    // FEAT-5: the user's reference library is the centre of the screen.
    similarPanel_.setBounds(b);
}

int ReferenceLibraryScreen::getNumRows() { return genreIds_.size(); }

void ReferenceLibraryScreen::paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height,
                                              bool rowIsSelected)
{
    if (rowNumber < 0 || rowNumber >= genreNames_.size()) return;
    g.fillAll(rowIsSelected ? KhensuTheme::bgRaised : KhensuTheme::bgBase);
    g.setColour(rowIsSelected ? KhensuTheme::textPrimary : KhensuTheme::textSecondary);
    g.setFont(KhensuTheme::bodyFont(12.0f));
    g.drawText(genreNames_[rowNumber], 8, 0, width - 16, height, juce::Justification::centredLeft);
}

void ReferenceLibraryScreen::selectedRowsChanged(int lastRowSelected)
{
    if (lastRowSelected < 0) return;
    selectedRow_ = lastRowSelected;
    refreshForSelectedGenre();
}

void ReferenceLibraryScreen::refreshStemEnergyLabel(const juce::String& genreId)
{
    auto entry = controller_.genreDatabase().findById(genreId.toStdString());
    if (!entry || entry->stem_energy_balance_pct.empty()) {
        stemEnergyLabel_.setText(TRANS("No explicit stem-energy targets for this genre."), juce::dontSendNotification);
        return;
    }

    juce::String text = "STEM ENERGY TARGETS\n";
    const juce::StringArray order{"vocals", "bass",  "kick",          "snare", "percussion",
                                  "drums",  "synth", "atmosphere_fx", "other"};
    for (const auto& bucket : order) {
        auto it = entry->stem_energy_balance_pct.find(bucket.toStdString());
        if (it == entry->stem_energy_balance_pct.end()) continue;
        // A bucket authored at exactly 0% (e.g. House's "drums": 0.0) means
        // its share was deliberately rolled into sibling sub-buckets already
        // itemized here (kick/snare/percussion) rather than double-counting -
        // showing "Drums: 0.0%" right next to those non-zero siblings reads
        // as a data bug rather than the intentional zero it is, so skip it.
        if (it->second == 0.0f) continue;
        // Capitalize / pretty-print bucket id.
        juce::String label = bucket.replace("_", "/").toUpperCase();
        if (label == "ATMOSPHERE/FX")
            label = "Atmosphere/FX";
        else
            label = label.substring(0, 1).toUpperCase() + label.substring(1).toLowerCase();
        text << label << ": " << juce::String(it->second, 1) << "%\n";
    }
    stemEnergyLabel_.setText(text, juce::dontSendNotification);
}

void ReferenceLibraryScreen::refreshForSelectedGenre()
{
    if (selectedRow_ < 0 || selectedRow_ >= genreIds_.size()) return;
    const juce::String genreId = genreIds_[selectedRow_];

    percentileLabel_.setText(TRANS("Loading reference data for %g...").replace("%g", genreNames_[selectedRow_]),
                             juce::dontSendNotification);
    refreshStemEnergyLabel(genreId);
    if (lastPointsGenre_ != genreId) userProfiles_.clear();
    profilesBar_.refresh(genreId); // CE-COL-4: the user's own profiles for this genre

    juce::Component::SafePointer<ReferenceLibraryScreen> self(this);
    controller_.lifecycle().getReferenceLibraryPoints(genreId,
                                          [self, genreId](std::vector<khensu::ipc::GenreReferencePoint> points) {
                                              auto* s = self.getComponent();
                                              if (s == nullptr) return;
                                              // A quick A -> B switch: A's late reply must
                                              // not paint A's points under B (UIX-20).
                                              if (s->selectedRow_ < 0 || s->selectedRow_ >= s->genreIds_.size()
                                                  || s->genreIds_[s->selectedRow_] != genreId)
                                                  return;
                                              s->rebuildCloud(genreId, std::move(points));
                                          });
}

void ReferenceLibraryScreen::rebuildCloud(const juce::String& genreId,
                                          std::vector<khensu::ipc::GenreReferencePoint> points)
{
    // CUT-6: the synthetic 3D reference cloud is gone; what stays is the
    // genre fit - where the mix sits against this genre's reference samples,
    // as numbers. The user's own references are the library below (FEAT-5).
    lastPointsGenre_ = genreId;
    lastPoints_ = points;
    if (points.empty()) {
        percentileLabel_.setText(TRANS("No reference data available for this genre yet."), juce::dontSendNotification);
        return;
    }
    juce::String text = TRANS("Genre: %g").replace("%g", genreNames_[juce::jlimit(0, genreNames_.size() - 1, selectedRow_)]) +
                        "\n" + TRANS("%n reference sample points").replace("%n", juce::String((int)points.size())) + "\n\n";
    if (controller_.hasAnalysis()) {
        const auto& analysis = controller_.currentAnalysis();
        // Share of the genre's samples below the mix, per metric.
        auto below = [&](auto field, double value) {
            int n = 0;
            for (const auto& p : points)
                if (field(p) < value) ++n;
            return juce::roundToInt(100.0 * n / (double)points.size());
        };
        text << TRANS("Your mix:") << " " << juce::String(analysis.loudness.lufs_integrated, 1) << " LUFS, "
             << juce::String(analysis.loudness.lra, 1) << " LU, " << juce::String(analysis.spectral.tilt_db_per_octave, 1)
             << " dB/oct, " << juce::String(analysis.dynamics.crest_factor_db, 1) << " dB crest\n\n"
             << TRANS("Louder than") << " " << below([](const auto& p) { return p.lufs_integrated; },
                                                    analysis.loudness.lufs_integrated)
             << "% " << TRANS("of them, more dynamic (LRA) than") << " "
             << below([](const auto& p) { return p.lra; }, analysis.loudness.lra) << "%, "
             << TRANS("brighter (tilt) than") << " "
             << below([](const auto& p) { return p.spectral_tilt_db_per_octave; },
                      analysis.spectral.tilt_db_per_octave)
             << "%.\n";
        if (analysis.benchmark && analysis.benchmark->is_bootstrap_data)
            text << "\n" << TRANS("(bootstrap/synthetic reference data)");
    } else {
        text << TRANS("Load a track to see where it sits in this genre.");
    }
    if (!userProfiles_.empty())
        text << "\n" << juce::String((int)userProfiles_.size()) << " " << TRANS("of your reference profiles are in this genre.");
    percentileLabel_.setText(text, juce::dontSendNotification);
}

} // namespace khensu
