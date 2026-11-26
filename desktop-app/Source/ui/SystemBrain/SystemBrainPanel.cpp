// desktop-app/Source/ui/SystemBrain/SystemBrainPanel.cpp
#include "SystemBrainPanel.h"
#include "../shared/PanelRunState.h"
#include "../shared/ServiceStatusText.h"
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include <khensu/systembrain/SystemBrainDatabase.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <thread>
#include <khensu/ipc/generated/ApiSchema.h>
#include <khensu/recommendations/SpectralRecommender.h>
#include <khensu/targets/GenreDatabase.h>

namespace af = khensu::api::fields;

namespace khensu {

const char* SystemBrainPanel::kCategories[] = {"All", nullptr};

// Every cached palette colour on this component, in one place (UI-01).
void SystemBrainPanel::applyThemeColours()
{
    serviceInfoLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    searchInput_.setColour(juce::TextEditor::backgroundColourId, KhensuTheme::bgRaised);
    searchInput_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
    searchInput_.setColour(juce::TextEditor::outlineColourId, KhensuTheme::borderSubtle);
    searchInput_.setColour(juce::TextEditor::focusedOutlineColourId, KhensuTheme::accentTeal);
}

SystemBrainPanel::SystemBrainPanel(IAppController& ctrl) : controller(ctrl)
{
    applyThemeColours();
    titleLabel_.setScreen(ScreenType::SystemBrain);
    addAndMakeVisible(titleLabel_);

    serviceInfoLabel_.setFont(KhensuTheme::bodyFont(type::caption));
    serviceInfoLabel_.setJustificationType(juce::Justification::centredRight);
    serviceInfoLabel_.setText(TRANS("Service info: querying..."), juce::dontSendNotification);
    addAndMakeVisible(serviceInfoLabel_);
    fetchServiceInfo();

    searchInput_.setFont(KhensuTheme::bodyFont(type::secondary));
    searchInput_.setTextToShowWhenEmpty(TRANS("Search features..."), KhensuTheme::textTertiary);
    searchInput_.onTextChange = [this]() {
        searchText_ = searchInput_.getText();
        startTimer(kSearchDebounceMs); // restarts on every keystroke
    };
    searchInput_.onReturnKey = [this]() {
        stopTimer();
        refreshFeatureList();
    };
    addAndMakeVisible(searchInput_);

    featureListViewport_.setViewedComponent(&listContent_, false);
    featureListViewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(featureListViewport_);

    detailViewport_.setViewedComponent(&detailPanel_, false);
    detailViewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(detailViewport_);

    rebuildCategoryChips();
    refreshFeatureList();
}

SystemBrainPanel::~SystemBrainPanel() { stopTimer(); }

void SystemBrainPanel::timerCallback()
{
    stopTimer();
    refreshFeatureList();
}

void SystemBrainPanel::rebuildCategoryChips()
{
    categoryChips_.clear();
    activeCategory_ = "";

    auto addChip = [this](const juce::String& label, const juce::String& category) {
        auto* chip = new juce::TextButton(label);
        chip->setClickingTogglesState(true);
        chip->setRadioGroupId(2101);
        chip->onClick = [this, category] {
            activeCategory_ = category;
            refreshFeatureList();
        };
        addAndMakeVisible(chip);
        categoryChips_.add(chip);
    };

    addChip(TRANS("All"), "");
    for (const auto& cat : controller.analysis().systemBrainDatabase().categories())
        addChip(juce::String(cat), juce::String(cat));

    if (auto* first = categoryChips_[0]) first->setToggleState(true, juce::dontSendNotification);
    resized();
}

void SystemBrainPanel::refreshFeatureList()
{
    const auto& db = controller.analysis().systemBrainDatabase();
    std::vector<FeatureEntry> results = db.search(searchText_.toStdString());

    if (activeCategory_.isNotEmpty()) {
        std::vector<FeatureEntry> filtered;
        const std::string cat = activeCategory_.toStdString();
        for (auto& e : results)
            if (e.category == cat) filtered.push_back(std::move(e));
        results = std::move(filtered);
    }

    cards_.clear();
    for (const auto& e : results) {
        auto card = std::make_unique<FeatureCardComponent>();
        card->setEntry(e);
        card->onSelect = [this](const std::string& id) { selectFeature(id); };

        if (controller.hasAnalysis()) {
            const auto& r = controller.currentAnalysis();
            GenreEntry genre;
            if (const auto* s = controller.getActiveSession())
                if (auto g = controller.genreDatabase().findById(s->target.primary_genre)) genre = *g;

            const std::string& id = e.id;
            auto setCard = [&](float val, float fallbackMin, float fallbackMax, float lo, float hi) {
                float minVal = e.visualization.hasVisualization() ? e.visualization.min : fallbackMin;
                float maxVal = e.visualization.hasVisualization() ? e.visualization.max : fallbackMax;
                card->setSessionValue(val, minVal, maxVal, lo, hi);
            };

            if (id == "integrated_loudness") {
                setCard(r.loudness.lufs_integrated, -30.0f, -4.0f, genre.lufs_range_min, genre.lufs_range_max);
            } else if (id == "loudness_range") {
                setCard(r.loudness.lra, 0.0f, 20.0f, genre.lra_range_min, genre.lra_range_max);
            } else if (id == "true_peak") {
                setCard(r.loudness.true_peak_dbtp, -6.0f, 2.0f, -6.0f, -1.0f);
            } else if (id == "peak_to_loudness_ratio") {
                setCard(r.loudness.plr, 0.0f, 20.0f, 8.0f, 15.0f);
            } else if (id == "dynamics_range") {
                setCard(r.dynamics.dynamic_range_db, 0.0f, 24.0f, 6.0f, 14.0f);
            } else if (id == "spectral_balance") {
                // The same window the low_end_heavy / low_end_light findings use.
                setCard(r.spectral.balance_low * 100.0f, 0.0f, 100.0f, SpectralRecommender::lowEndFloor(genre) * 100.0f,
                        SpectralRecommender::lowEndCeiling(genre) * 100.0f);
            } else if (id == "stereo_field") {
                setCard(r.stereo.width, 0.0f, 1.0f, genre.stereo_width_min, genre.stereo_width_max);
            } else if (id == "phase_correlation") {
                setCard(r.stereo.correlation, -1.0f, 1.0f, 0.0f, 1.0f);
            } else if (id == "harshness_detection" && r.harshness.has_value()) {
                setCard(r.harshness->score * 100.0f, 0.0f, 100.0f, 0.0f, 35.0f);
            } else if (id == "mono_compatibility_analysis" && r.mono_compat.has_value()) {
                setCard(r.mono_compat->low_end_stereo_pct, 0.0f, 100.0f, 0.0f, 20.0f);
            } else if (id == "resonance_detection" && r.resonance.has_value() && !r.resonance->hotspots.empty()) {
                // Hotspots are sorted by prominence - the loudest one is the
                // headline number, matching the doc's own "fires above 10 dB" cue.
                setCard(r.resonance->hotspots.front().prominence, 0.0f, 20.0f, 0.0f, 10.0f);
            } else if (id == "masking_collision" && r.masking.has_value() && !r.masking->pairs.empty()) {
                float worst = 0.0f;
                for (const auto& pair : r.masking->pairs)
                    worst = juce::jmax(worst, pair.collision_score);
                setCard(worst * 100.0f, 0.0f, 100.0f, 0.0f, 40.0f);
            }
        }

        listContent_.addAndMakeVisible(*card);
        cards_.push_back(std::move(card));
    }
    layoutCards();
    repaint();
}

void SystemBrainPanel::selectFeature(const std::string& id)
{
    if (auto e = controller.analysis().systemBrainDatabase().findById(id)) {
        detailPanel_.setEntry(*e);
        applySessionValue(id);
    }
}

void SystemBrainPanel::applySessionValue(const std::string& id)
{
    // Feed the detail panel's "YOUR SESSION VALUE" section from the live
    // analysis. Previously hasSessionValue_ was never set, leaving the whole
    // section (meter included) permanently dead.
    if (!controller.hasAnalysis()) return;
    const auto& r = controller.currentAnalysis();

    GenreEntry genre;
    if (const auto* s = controller.getActiveSession())
        if (auto g = controller.genreDatabase().findById(s->target.primary_genre)) genre = *g;

    auto f1 = [](float v) { return juce::String(v, 1); };

    if (id == "integrated_loudness") {
        detailPanel_.setSessionValue(f1(r.loudness.lufs_integrated), "LUFS",
                                     f1(genre.lufs_range_min) + " to " + f1(genre.lufs_range_max) + " LUFS",
                                     r.loudness.lufs_integrated, -30.0f, -4.0f, genre.lufs_range_min,
                                     genre.lufs_range_max);
    } else if (id == "loudness_range") {
        detailPanel_.setSessionValue(f1(r.loudness.lra), "LU",
                                     f1(genre.lra_range_min) + " to " + f1(genre.lra_range_max) + " LU", r.loudness.lra,
                                     0.0f, 20.0f, genre.lra_range_min, genre.lra_range_max);
    } else if (id == "true_peak") {
        detailPanel_.setSessionValue(f1(r.loudness.true_peak_dbtp), "dBTP",
                                     juce::String("<= ") + f1(genre.true_peak_max) + " dBTP", r.loudness.true_peak_dbtp,
                                     -12.0f, 3.0f, -12.0f, genre.true_peak_max);
    } else if (id == "peak_to_loudness_ratio") {
        detailPanel_.setSessionValue(f1(r.loudness.plr), "dB", "8-16 dB typical for a dynamic master", r.loudness.plr,
                                     0.0f, 24.0f, 8.0f, 16.0f);
    } else if (id == "dynamics_range") {
        detailPanel_.setSessionValue(f1(r.dynamics.dynamic_range_db), "dB",
                                     "crest " + f1(r.dynamics.crest_factor_db) + " dB", r.dynamics.dynamic_range_db,
                                     0.0f, 24.0f, 6.0f, 14.0f);
    } else if (id == "spectral_balance") {
        detailPanel_.setSessionValue(f1(r.spectral.balance_low * 100.0f), "% low-end energy",
                                     f1(genre.low_end_weight * 100.0f) + "% for this genre",
                                     r.spectral.balance_low * 100.0f, 0.0f, 100.0f,
                                     SpectralRecommender::lowEndFloor(genre) * 100.0f,
                                     SpectralRecommender::lowEndCeiling(genre) * 100.0f);
    } else if (id == "stereo_field") {
        detailPanel_.setSessionValue(f1(r.stereo.width), "width (0=mono, 1=wide)",
                                     f1(genre.stereo_width_min) + " to " + f1(genre.stereo_width_max), r.stereo.width,
                                     0.0f, 1.0f, genre.stereo_width_min, genre.stereo_width_max);
    } else if (id == "phase_correlation") {
        detailPanel_.setSessionValue(f1(r.stereo.correlation), "correlation", "> 0 mono-safe; < 0 phase risk",
                                     r.stereo.correlation, -1.0f, 1.0f, 0.0f, 1.0f);
    } else if (id == "harshness_detection" && r.harshness.has_value()) {
        detailPanel_.setSessionValue(f1(r.harshness->score * 100.0f), "/100", "< 35 comfortable",
                                     r.harshness->score * 100.0f, 0.0f, 100.0f, 0.0f, 35.0f);
    } else if (id == "mono_compatibility_analysis" && r.mono_compat.has_value()) {
        detailPanel_.setSessionValue(f1(r.mono_compat->low_end_stereo_pct), "% stereo low-end",
                                     "< 20% keeps bass mono-solid", r.mono_compat->low_end_stereo_pct, 0.0f, 100.0f,
                                     0.0f, 20.0f);
    } else if (id == "ai_artifact_detection") {
        // AUDIT-010: the analysis pass does not assess this - say so, not "0%".
        detailPanel_.setSessionNote(TRANS("Not assessed here"),
                                    TRANS("Run the AI Detection screen for a calibrated, evidence-based check."));
    } else if (id == "resonance_detection" && r.resonance.has_value() && !r.resonance->hotspots.empty()) {
        const auto& top = r.resonance->hotspots.front();
        detailPanel_.setSessionValue(f1(top.prominence), "dB prominence @ " + f1(top.hz) + " Hz",
                                     "> 10 dB fires the resonance recommender", top.prominence, 0.0f, 20.0f, 0.0f,
                                     10.0f);
    } else if (id == "masking_collision" && r.masking.has_value() && !r.masking->pairs.empty()) {
        const MaskingPairSummary* worst = &r.masking->pairs.front();
        for (const auto& pair : r.masking->pairs)
            if (pair.collision_score > worst->collision_score) worst = &pair;
        detailPanel_.setSessionValue(f1(worst->collision_score * 100.0f) + "%",
                                     juce::String(worst->stem_a) + " / " + juce::String(worst->stem_b),
                                     ">= 65% is a red-risk collision", worst->collision_score * 100.0f, 0.0f, 100.0f,
                                     0.0f, 40.0f);
    }
    // Other feature ids describe app concepts (dashboards, engines) with no
    // single session number - the section simply stays hidden for those.
}

void SystemBrainPanel::layoutCards()
{
    const int w = juce::jmax(0, featureListViewport_.getWidth() - 10);
    if (w == 0) {
        listContent_.setSize(0, 0);
        return;
    }

    // §6: grid fashion - two columns when the list is wide enough, one on
    // narrow windows. The viewport keeps the whole grid scrollable.
    const int gap = 6;
    const int cols = w >= 420 ? 2 : 1;
    const int cw = (w - (cols - 1) * gap) / cols;

    int y = 0, rowH = 0, col = 0;
    for (auto& c : cards_) {
        const int h = c->preferredHeight();
        c->setBounds(col * (cw + gap), y, cw, h);
        rowH = juce::jmax(rowH, h);
        if (++col == cols) {
            col = 0;
            y += rowH + gap;
            rowH = 0;
        }
    }
    if (col != 0) y += rowH + gap;
    listContent_.setSize(w, y);
}

void SystemBrainPanel::fetchServiceInfo()
{
    fetchQuietly(*this, controller, []() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.getServiceMetadata();
        return result;
    }, [](SystemBrainPanel& self, auto&& result) {
        if (result.is_err()) {
            const bool engineDown =
                self.controller.lifecycle().getPythonServiceStatus() != PythonStatus::Running;
            self.serviceInfoLabel_.setText(engineDown ? service_status::waitingPlaceholder()
                                                       : TRANS("Engine details are unavailable right now."),
                                            juce::dontSendNotification);
            return;
        }
        auto* obj = result.value().getDynamicObject();
        if (obj == nullptr) return;
        self.serviceInfoLabel_.setText(
            TRANS("Service v") + obj->getProperty(af::AnalysisMetadataResponse::service_version).toString() + "  -  Python " +
                obj->getProperty(af::AnalysisMetadataResponse::python_version).toString() + "  -  librosa " +
                obj->getProperty(af::AnalysisMetadataResponse::librosa_version).toString() + "  -  schema " +
                obj->getProperty(af::AnalysisMetadataResponse::analysis_schema_version).toString() + "  -  genre heuristics v" +
                obj->getProperty(af::AnalysisMetadataResponse::genre_heuristic_version).toString(),
            juce::dontSendNotification);
    }, nullptr, true);
}

void SystemBrainPanel::paint(juce::Graphics& g) { KhensuLookAndFeel::drawGlassmorphicBackground(g, getLocalBounds()); }

void SystemBrainPanel::resized()
{
    auto b = getLocalBounds().reduced(KhensuTheme::panelPadding, KhensuTheme::sp2);

    // Screen header - title left, live service-version diagnostics right
    auto headerRow = b.removeFromTop(KhensuTheme::headerH);
    serviceInfoLabel_.setBounds(headerRow.removeFromRight(headerRow.getWidth() * 55 / 100));
    titleLabel_.setBounds(headerRow);
    b.removeFromTop(KhensuTheme::sp2);

    // Left 38%: search + filter pills + feature list
    auto left = b.removeFromLeft(b.getWidth() * 38 / 100);
    left.removeFromRight(KhensuTheme::cardGap);

    searchInput_.setBounds(left.removeFromTop(36));
    left.removeFromTop(KhensuTheme::sp2);

    // Category filter pills - flow-wrapped onto as many rows as needed so no
    // chip clips off the narrow (38%) column. Previously they were laid out on
    // a single fixed 30px row and the later categories ran off the right edge.
    const int chipH = 28, chipGap = 8, rowGap = 6;
    auto chipWidth = [](juce::Button* c) { return juce::jmax(60, c->getButtonText().length() * 9 + 24); };
    if (!categoryChips_.isEmpty()) {
        const int colW = left.getWidth();
        int rows = 1, rowX = 0;
        for (auto* chip : categoryChips_) {
            const int cw = chipWidth(chip);
            if (rowX > 0 && rowX + cw > colW) {
                ++rows;
                rowX = 0;
            }
            rowX += cw + chipGap;
        }
        auto chipArea = left.removeFromTop(rows * chipH + (rows - 1) * rowGap);
        left.removeFromTop(KhensuTheme::sp2);
        int px = chipArea.getX(), py = chipArea.getY();
        for (auto* chip : categoryChips_) {
            const int cw = chipWidth(chip);
            if (px > chipArea.getX() && px + cw > chipArea.getRight()) {
                px = chipArea.getX();
                py += chipH + rowGap;
            }
            chip->setBounds(px, py, cw, chipH);
            px += cw + chipGap;
        }
    }

    // Feature list viewport fills remaining left column
    featureListViewport_.setBounds(left);

    // Right 62%: 5-section detail panel inside a scroll viewport, sized to
    // its measured content height so nothing overlaps or truncates (§6).
    detailViewport_.setBounds(b);
    {
        const int dw = juce::jmax(0, detailViewport_.getWidth() - 12);
        detailPanel_.setSize(dw, juce::jmax(detailViewport_.getHeight(), detailPanel_.preferredHeight(dw)));
    }

    layoutCards();
}

void SystemBrainPanel::updateState() { refreshFeatureList(); }

} // namespace khensu
