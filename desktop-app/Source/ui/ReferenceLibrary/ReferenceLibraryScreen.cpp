#include "ReferenceLibraryScreen.h"
#include "../shared/KhensuTheme.h"
#include "../../IAppController.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <khensu/ipc/generated/ApiSchema.h>
#include <khensu/targets/GenreDatabase.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

namespace {
constexpr int kIdAll = 1;
constexpr int kIdUnsorted = 2;
} // namespace

// Every cached palette colour on this component, in one place (UI-01).
void ReferenceLibraryScreen::applyThemeColours()
{
    percentileLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    stemEnergyLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    genreCaption_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    genreHint_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

ReferenceLibraryScreen::ReferenceLibraryScreen(IAppController& controller, const std::string& baseUrl)
    : controller_(controller), scanPanel_(controller, baseUrl), similarPanel_(controller, baseUrl),
      profilesBar_(controller, [this] { return selectedGenreId(); }, baseUrl)
{
    applyThemeColours();
    addAndMakeVisible(scanPanel_);
    addAndMakeVisible(similarPanel_);
    addAndMakeVisible(profilesBar_);
    addAndMakeVisible(fitChart_);
    profilesBar_.onProfilesChanged = [this](const juce::String& genreId,
                                            std::vector<ReferenceProfilesBar::UserProfile> profiles) {
        userProfiles_ = std::move(profiles);
        if (genreId == lastPointsGenre_ && !lastPoints_.empty()) rebuildCloud(lastPointsGenre_, lastPoints_);
    };
    titleLabel_.setScreen(ScreenType::ReferenceLibrary);
    addAndMakeVisible(titleLabel_);

    genreCaption_.setText(TRANS("Genre"), juce::dontSendNotification);
    genreCaption_.setFont(KhensuTheme::labelFont(type::body));
    addAndMakeVisible(genreCaption_);
    genrePicker_.setTitle(TRANS("Genre"));
    genrePicker_.setTooltip(TRANS("Pick a genre: its scanned songs and samples, its norms and your mix against them"));
    genrePicker_.onChange = [this] { pickerChanged(); };
    addAndMakeVisible(genrePicker_);
    genreHint_.setText(TRANS("Shows the songs and samples sorted into the genre, its loudness and stem targets, "
                             "and where your mix sits against them."),
                       juce::dontSendNotification);
    genreHint_.setFont(KhensuTheme::bodyFont(type::caption));
    genreHint_.setMinimumHorizontalScale(0.85f);
    addAndMakeVisible(genreHint_);

    percentileLabel_.setFont(KhensuTheme::bodyFont(type::secondary));
    percentileLabel_.setJustificationType(juce::Justification::topLeft);
    percentileLabel_.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(percentileLabel_);

    stemEnergyLabel_.setFont(KhensuTheme::bodyFont(type::caption));
    stemEnergyLabel_.setJustificationType(juce::Justification::topLeft);
    stemEnergyLabel_.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(stemEnergyLabel_);

    fitChart_.onGenreClicked = [this](const juce::String& id) { selectGenre(id); };

    scanPanel_.onSummary = [this](const juce::var& summary) {
        counts_ = summary;
        rebuildPicker();
    };
    scanPanel_.onScanFinished = [this] { similarPanel_.showLibrary(); };

    rebuildPicker();
}

ReferenceLibraryScreen::~ReferenceLibraryScreen() = default;

// --- the genre picker ----------------------------------------------------------

void ReferenceLibraryScreen::rebuildPicker()
{
    const auto& db = controller_.genreDatabase();
    auto count = [this](const char* key, const juce::String& id) {
        const juce::var m = counts_[key];
        return m.isObject() ? (int)m[juce::Identifier(id)] : 0;
    };
    auto withCount = [](const juce::String& name, int n) {
        return n > 0 ? name + " (" + juce::String(n) + ")" : name;
    };

    // Families in display order, their genres under each.
    std::map<juce::String, std::vector<const GenreEntry*>> byFamily;
    for (const auto& g : db.listAll())
        byFamily[juce::String(g.parent.empty() ? g.id : g.parent)].push_back(&g);
    std::vector<juce::String> families;
    for (const auto& [fam, _] : byFamily)
        families.push_back(fam);
    std::sort(families.begin(), families.end(), [&db](const juce::String& a, const juce::String& b) {
        return referenceGenreName(db, a).compareIgnoreCase(referenceGenreName(db, b)) < 0;
    });

    const PickerChoice keep = (selectedId_ >= 1 && selectedId_ <= (int)choices_.size())
                                  ? choices_[(size_t)(selectedId_ - 1)]
                                  : PickerChoice{};
    choices_.clear();
    genrePicker_.clear(juce::dontSendNotification);
    choices_.push_back({});
    genrePicker_.addItem(TRANS("All genres"), kIdAll);
    choices_.push_back({{}, {}, true});
    const int unsorted = counts_.isObject() ? (int)counts_[af::ReferenceScanSummaryResponse::unsorted] : 0;
    genrePicker_.addItem(withCount(TRANS("Unsorted"), unsorted), kIdUnsorted);
    for (const auto& fam : families) {
        genrePicker_.addSectionHeading(referenceGenreName(db, fam));
        choices_.push_back({{}, fam, false});
        genrePicker_.addItem(withCount(TRANS("All %f").replace("%f", referenceGenreName(db, fam)),
                                       count(af::ReferenceScanSummaryResponse::families, fam)),
                             (int)choices_.size());
        auto members = byFamily[fam];
        std::sort(members.begin(), members.end(), [](const GenreEntry* a, const GenreEntry* b) {
            return juce::String(a->name).compareIgnoreCase(juce::String(b->name)) < 0;
        });
        for (const auto* g : members) {
            if (juce::String(g->id) == fam) continue; // "All <family>" covers it
            choices_.push_back({juce::String(g->id), {}, false});
            genrePicker_.addItem(withCount(juce::String(g->name),
                                           count(af::ReferenceScanSummaryResponse::genres, juce::String(g->id))),
                                 (int)choices_.size());
        }
    }
    // Keep the pick across a rebuild (new counts).
    int id = kIdAll;
    for (size_t i = 0; i < choices_.size(); ++i)
        if (choices_[i].genre == keep.genre && choices_[i].family == keep.family
            && choices_[i].unsorted == keep.unsorted)
            id = (int)i + 1;
    selectedId_ = id;
    genrePicker_.setSelectedId(id, juce::dontSendNotification);
}

juce::String ReferenceLibraryScreen::selectedGenreId() const
{
    if (selectedId_ < 1 || selectedId_ > (int)choices_.size()) return {};
    const auto& c = choices_[(size_t)(selectedId_ - 1)];
    if (c.genre.isNotEmpty()) return c.genre;
    if (c.family.isNotEmpty() && controller_.genreDatabase().findById(c.family.toStdString())) return c.family;
    return {};
}

void ReferenceLibraryScreen::selectGenre(const juce::String& id)
{
    for (size_t i = 0; i < choices_.size(); ++i) {
        const auto& c = choices_[i];
        if ((c.genre.isNotEmpty() && c.genre == id) || (c.genre.isEmpty() && !c.unsorted && c.family == id)) {
            genrePicker_.setSelectedId((int)i + 1, juce::dontSendNotification);
            pickerChanged();
            return;
        }
    }
}

void ReferenceLibraryScreen::pickerChanged()
{
    const int id = genrePicker_.getSelectedId();
    if (id < 1 || id > (int)choices_.size()) return;
    selectedId_ = id;
    const auto& c = choices_[(size_t)(id - 1)];
    ReferenceScanPanel::GenreScope scope;
    scope.genre = c.genre;
    scope.family = c.family;
    scope.unsorted = c.unsorted;
    scanPanel_.setGenreScope(scope);
    fitChart_.setSelectedGenre(selectedGenreId());
    refreshForSelectedGenre();
}

// --- showing the screen -----------------------------------------------------------

void ReferenceLibraryScreen::visibilityChanged()
{
    if (!isVisible()) return;
    similarPanel_.showLibrary(); // FEAT-5: the user's references, listed on open
    scanPanel_.refresh();        // REFS-1: the folder, its counts, a running scan
    computeGenreRanking();
    if (pickerInitialised_) return;
    pickerInitialised_ = true;

    // The first time: the loaded song's own (detected or chosen) genre, if
    // any; else every genre.
    juce::String defaultGenreId;
    if (controller_.hasAnalysis() && controller_.currentAnalysis().benchmark)
        defaultGenreId = juce::String(controller_.currentAnalysis().benchmark->genre_id);
    else if (auto* session = controller_.getActiveSession())
        defaultGenreId = juce::String(session->target.primary_genre);
    if (defaultGenreId.isNotEmpty()) {
        selectGenre(defaultGenreId);
        if (selectedGenreId() == defaultGenreId) return;
    }
    genrePicker_.setSelectedId(kIdAll, juce::dontSendNotification);
    pickerChanged();
}

void ReferenceLibraryScreen::computeGenreRanking()
{
    if (!controller_.hasAnalysis()) {
        fitChart_.setEntries({}, TRANS("Load and analyse a track to see which genres it sits closest to."));
        return;
    }

    const auto& analysis = controller_.currentAnalysis();
    const float songLufs = analysis.loudness.lufs_integrated;
    const float songLra = analysis.loudness.lra;
    const float songTilt = analysis.spectral.tilt_db_per_octave;
    const float songCrest = analysis.dynamics.crest_factor_db;

    // A normalised Euclidean distance to each genre's targets (GenreDatabase
    // already stores these per genre - no backend call): "how close does
    // this mix sit to that genre's typical numbers", not a claim about real
    // reference tracks.
    std::vector<GenreFitChart::Entry> entries;
    for (const auto& g : controller_.genreDatabase().listAll()) {
        const float genreLra = (g.lra_range_min + g.lra_range_max) * 0.5f;
        const float dLufs = (songLufs - g.lufs_target) / 3.0f;
        const float dLra = (songLra - genreLra) / 4.0f;
        const float dTilt = (songTilt - g.spectral_tilt_target_db_per_octave) / 3.0f;
        const float dCrest = (songCrest - g.crest_factor_target_db) / 4.0f;
        const float dist = std::sqrt(dLufs * dLufs + dLra * dLra + dTilt * dTilt + dCrest * dCrest);
        entries.push_back({juce::String(g.id), juce::String(g.name), dist});
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.distance < b.distance; });
    fitChart_.setEntries(std::move(entries));
    fitChart_.setSelectedGenre(selectedGenreId());
}

void ReferenceLibraryScreen::paint(juce::Graphics& g) { g.fillAll(KhensuTheme::bgBase); }

void ReferenceLibraryScreen::resized()
{
    auto b = getLocalBounds().reduced(12);
    titleLabel_.setBounds(b.removeFromTop(28));
    b.removeFromTop(6);
    profilesBar_.setBounds(b.removeFromTop(44)); // CE-COL-4
    b.removeFromTop(4);

    auto pickerRow = b.removeFromTop(KhensuTheme::buttonHSmall);
    genreCaption_.setBounds(pickerRow.removeFromLeft(56));
    genrePicker_.setBounds(pickerRow.removeFromLeft(260));
    pickerRow.removeFromLeft(10);
    genreHint_.setBounds(pickerRow);
    b.removeFromTop(8);

    auto right = b.removeFromRight(juce::jlimit(260, 340, b.getWidth() / 4));
    b.removeFromRight(10);
    auto chart = right.removeFromBottom(juce::jmax(250, right.getHeight() * 45 / 100));
    fitChart_.setBounds(chart);
    right.removeFromBottom(8);
    percentileLabel_.setBounds(right.removeFromTop(right.getHeight() * 55 / 100));
    right.removeFromTop(6);
    stemEnergyLabel_.setBounds(right);

    // REFS-1 above, the user's reference library (FEAT-5) below.
    auto scan = b.removeFromTop(b.getHeight() * 60 / 100);
    scanPanel_.setBounds(scan);
    b.removeFromTop(8);
    similarPanel_.setBounds(b);
}

void ReferenceLibraryScreen::refreshStemEnergyLabel(const juce::String& genreId)
{
    if (genreId.isEmpty()) {
        stemEnergyLabel_.setText({}, juce::dontSendNotification);
        return;
    }
    auto entry = controller_.genreDatabase().findById(genreId.toStdString());
    if (!entry || entry->stem_energy_balance_pct.empty()) {
        stemEnergyLabel_.setText(TRANS("No explicit stem-energy targets for this genre."), juce::dontSendNotification);
        return;
    }

    juce::String text = TRANS("Stem energy targets") + "\n";
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
    const juce::String genreId = selectedGenreId();
    refreshStemEnergyLabel(genreId);
    if (lastPointsGenre_ != genreId) userProfiles_.clear();
    profilesBar_.refresh(genreId); // CE-COL-4: the user's own profiles for this genre
    if (genreId.isEmpty()) {
        lastPointsGenre_.clear();
        lastPoints_.clear();
        percentileLabel_.setText(TRANS("Pick a genre to see its norms and where your mix sits in it."),
                                 juce::dontSendNotification);
        return;
    }
    const juce::String name = referenceGenreName(controller_.genreDatabase(), genreId);
    percentileLabel_.setText(TRANS("Loading reference data for %g...").replace("%g", name),
                             juce::dontSendNotification);

    juce::Component::SafePointer<ReferenceLibraryScreen> self(this);
    controller_.lifecycle().getReferenceLibraryPoints(genreId,
                                                      [self, genreId](std::vector<khensu::ipc::GenreReferencePoint> points) {
                                                          auto* s = self.getComponent();
                                                          if (s == nullptr) return;
                                                          // A quick A -> B switch: A's late reply must
                                                          // not paint A's points under B (UIX-20).
                                                          if (s->selectedGenreId() != genreId) return;
                                                          s->rebuildCloud(genreId, std::move(points));
                                                      });
}

void ReferenceLibraryScreen::rebuildCloud(const juce::String& genreId,
                                          std::vector<khensu::ipc::GenreReferencePoint> points)
{
    // CUT-6: the synthetic 3D reference cloud is gone; what stays is the
    // genre fit - where the mix sits against this genre's reference samples,
    // as numbers.
    lastPointsGenre_ = genreId;
    lastPoints_ = points;
    if (points.empty()) {
        percentileLabel_.setText(TRANS("No reference data available for this genre yet."), juce::dontSendNotification);
        return;
    }
    const juce::String name = referenceGenreName(controller_.genreDatabase(), genreId);
    juce::String text = TRANS("Genre: %g").replace("%g", name) + "\n" +
                        TRANS("%n reference sample points").replace("%n", juce::String((int)points.size())) + "\n\n";
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
