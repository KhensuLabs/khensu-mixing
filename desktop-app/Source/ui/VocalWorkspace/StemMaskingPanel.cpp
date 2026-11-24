// desktop-app/Source/ui/VocalWorkspace/StemMaskingPanel.cpp
#include "StemMaskingPanel.h"
#include "../shared/PanelRunState.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../../IAppController.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/analysis/MaskingSummary.h>
#include <thread>
#include "../shared/KhensuTheme.h"
#include "../shared/ServiceErrorHint.h"
#include "../shared/FloatVector.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

namespace {
constexpr std::pair<int, int> kPairIndices[6] = {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}};
const char* const kStemNames[4] = {"vocals", "drums", "bass", "other"};
} // namespace

void StemMaskingPanel::Cell::paint(juce::Graphics& g)
{
    g.setColour(KhensuTheme::bgSurface);
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(2.0f), 4.0f);

    auto b = getLocalBounds().reduced(6, 4);
    g.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::bold));
    g.setColour(KhensuTheme::textSecondary);
    g.drawText(stemA.toUpperCase() + " / " + stemB.toUpperCase(), b.removeFromTop(14), juce::Justification::centredLeft,
               true);

    if (score01 < 0.0) {
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(khensu::KhensuTheme::uiFont(11.0f, juce::Font::plain));
        g.drawText("--", b, juce::Justification::centredLeft, false);
        return;
    }

    auto barArea = b.removeFromTop(16);
    g.setColour(KhensuTheme::bgOverlay);
    g.fillRoundedRectangle(barArea.toFloat(), 3.0f);
    auto fillArea = barArea.withWidth(juce::roundToInt(barArea.getWidth() * (float)score01));
    g.setColour(riskColour(score01));
    g.fillRoundedRectangle(fillArea.toFloat(), 3.0f);

    g.setColour(KhensuTheme::textPrimary);
    g.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::bold));
    g.drawText(juce::String(juce::roundToInt(score01 * 100.0)) + "% collision risk", b.removeFromTop(14),
               juce::Justification::centredLeft, false);
}

juce::Colour StemMaskingPanel::riskColour(double score01)
{
    if (score01 >= 0.66) return KhensuTheme::accentRed;
    if (score01 >= 0.33) return KhensuTheme::accentAmber;
    return KhensuTheme::accentTeal;
}

void StemMaskingPanel::CollisionTimeStrip::paint(juce::Graphics& g)
{
    g.setFont(khensu::KhensuTheme::uiFont(9.5f, juce::Font::bold));
    g.setColour(KhensuTheme::textSecondary);
    auto b = getLocalBounds();
    g.drawText(pairLabel.isEmpty() ? "Click a pair above to see collisions over time" : pairLabel, b.removeFromTop(14),
               juce::Justification::centredLeft, true);

    if (windows.empty() || totalDuration <= 0.0) return;

    auto strip = b.toFloat();
    for (const auto& w : windows) {
        const float x0 = strip.getX() + (float)(w.start_sec / totalDuration) * strip.getWidth();
        const float x1 = strip.getX() + (float)(w.end_sec / totalDuration) * strip.getWidth();
        g.setColour(riskColour(w.collision_score));
        g.fillRect(x0, strip.getY(), juce::jmax(1.0f, x1 - x0 - 1.0f), strip.getHeight());
    }
}

// Every cached palette colour on this component, in one place (UI-01).
void StemMaskingPanel::applyThemeColours()
{
    fixSuggestionLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    audibilityLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

StemMaskingPanel::StemMaskingPanel(IAppController& c) : PanelCard(c, "Masking & Frequency Collision")
{
    applyThemeColours();
    setExplainer(TRANS("Masking & Frequency Collision"), TRANS("Scores how much each pair of separated stems occupies the same "
                                                  "frequency space. High collision risk means the two elements are "
                                                  "likely to fight for the same band and mask each other - a common "
                                                  "cause of muddiness. Requires stems to be isolated first."));

    for (size_t i = 0; i < cells_.size(); ++i) {
        cells_[i].stemA = kStemNames[kPairIndices[i].first];
        cells_[i].stemB = kStemNames[kPairIndices[i].second];
        cells_[i].onClicked = [this, i]() { selectPairForTimeStrip(cells_[i]); };
        addAndMakeVisible(cells_[i]);
    }

    addAndMakeVisible(timeStrip_);
    addAndMakeVisible(heatmapView_);

    fixSuggestionLabel_.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::plain));
    fixSuggestionLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(fixSuggestionLabel_);

    generateFixButton_.setEnabled(false);
    generateFixButton_.onClick = [this]() { generateFixChain(); };
    addAndMakeVisible(generateFixButton_);

    analyzeButton_.onClick = [this]() { runAnalysis(); };
    analyzeButton_.setEnabled(false);
    addAndMakeVisible(analyzeButton_);

    statusLabel_.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::plain));
    addAndMakeVisible(statusLabel_);

    separationInfoLabel_.setFont(khensu::KhensuTheme::uiFont(9.5f, juce::Font::plain));
    addAndMakeVisible(separationInfoLabel_);

    audibilityLabel_.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::plain));
    audibilityLabel_.setJustificationType(juce::Justification::topLeft);
    audibilityLabel_.setMinimumHorizontalScale(1.0f);
    audibilityLabel_.setTooltip(TRANS("Partial loudness (Moore-Glasberg): how loud the vocal stays inside the mix, "
                                      "compared with how loud it is on its own"));
    addAndMakeVisible(audibilityLabel_);
}

void StemMaskingPanel::runVocalAudibility()
{
    const juce::String vocals = stemPaths_[0];
    const juce::String mix = controller_.activeSourcePath();
    if (vocals.isEmpty() || mix.isEmpty()) return;
    audibilityLabel_.setText(TRANS("Measuring vocal loudness in the mix..."), juce::dontSendNotification);
    juce::Component::SafePointer<StemMaskingPanel> self(this);
    controller_.runInBackground([self, &work = controller_, vocals, mix]() {
        auto result = ipc::HttpServiceBridge{}.partialLoudness(vocals, mix);
        postIfCurrent(self, work, [result](StemMaskingPanel& panel) {
            if (result.is_err()) {
                panel.audibilityLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
                panel.audibilityLabel_.setText(TRANS("Vocal audibility unavailable: ") +
                                                   classifyServiceError(result.error()).message,
                                               juce::dontSendNotification);
                return;
            }
            const juce::var v = result.value();
            const double alone = (double)v[af::PartialLoudnessResponse::target_loudness_sone];
            const double inMix = (double)v[af::PartialLoudnessResponse::target_partial_loudness_sone];
            const double ratio = (double)v[af::PartialLoudnessResponse::partial_ratio];
            const juce::String verdict = v[af::PartialLoudnessResponse::verdict].toString();
            juce::String text = TRANS("Vocal in the mix: ") + juce::String(inMix, 1) + TRANS(" of ") +
                                juce::String(alone, 1) + TRANS(" sone (") +
                                juce::String(juce::roundToInt(ratio * 100.0)) + "%) - " +
                                (verdict == "buried"      ? TRANS("buried")
                                 : verdict == "competing" ? TRANS("competing")
                                                          : TRANS("clear"));
            const juce::var worst = v[af::PartialLoudnessResponse::worst_band];
            if (worst.isObject() && (double)worst[af::PartialLoudnessBand::ratio] < 0.5)
                text << TRANS("; most masked ") << juce::String((int)(double)worst[af::PartialLoudnessBand::lo_hz])
                     << "-" << juce::String((int)(double)worst[af::PartialLoudnessBand::hi_hz]) << " Hz";
            // Transient state tint (re-resolved on the next measurement).
            panel.audibilityLabel_.setColour(juce::Label::textColourId,
                                             verdict == "buried"      ? KhensuTheme::accentRed
                                             : verdict == "competing" ? KhensuTheme::accentAmber
                                                                      : KhensuTheme::accentTeal);
            panel.audibilityLabel_.setText(text, juce::dontSendNotification);
        });
    });
}

void StemMaskingPanel::setSeparationInfo(const juce::String& method, float qualityScore)
{
    if (method.isEmpty() || qualityScore < 0.0f) {
        separationInfoLabel_.setText(TRANS("Separation method/quality unknown (stems found on disk from a previous run)."),
                                     juce::dontSendNotification);
        separationInfoLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
        return;
    }

    juce::String text =
        method.toUpperCase() + " separation, quality " + juce::String(juce::roundToInt(qualityScore * 100.0f)) + "%";
    juce::Colour col;
    if (qualityScore < 0.5f) {
        text += " - collision scores below may be unreliable (high stem leakage)";
        col = KhensuTheme::accentRed;
    } else if (qualityScore < 0.75f) {
        text += " - some stem leakage possible";
        col = KhensuTheme::accentAmber;
    } else {
        col = KhensuTheme::accentTeal;
    }
    separationInfoLabel_.setText(text, juce::dontSendNotification);
    separationInfoLabel_.setColour(juce::Label::textColourId, col);
}

void StemMaskingPanel::setStemPaths(const juce::String& vocals, const juce::String& drums, const juce::String& bass,
                                    const juce::String& other)
{
    if (stemPaths_[0] == vocals && stemPaths_[1] == drums && stemPaths_[2] == bass && stemPaths_[3] == other) {
        return;
    }

    stemPaths_[0] = vocals;
    stemPaths_[1] = drums;
    stemPaths_[2] = bass;
    stemPaths_[3] = other;

    const bool hasStems = vocals.isNotEmpty() || drums.isNotEmpty() || bass.isNotEmpty() || other.isNotEmpty();
    analyzeButton_.setEnabled(hasStems);

    for (auto& cell : cells_) {
        cell.score01 = -1.0;
        cell.repaint();
    }

    // Auto-runs now that router_masking.py caches by (path, mtime, size) per
    // stem (see its own history) -- a repeat visit to this track is free
    // server-side, and the original "no consent" concern was about surprise
    // CPU cost, which the cache already eliminates for anything but the
    // first pass. The button stays for manual re-analysis after the audio
    // changes underneath an already-cached path.
    if (hasStems) {
        statusLabel_.setText(TRANS("Stems ready. Analyzing frequency collisions..."), juce::dontSendNotification);
        runAnalysis();
        runVocalAudibility();
    }
}

void StemMaskingPanel::runAnalysis()
{
    analyzeButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Analyzing stem frequency overlap..."), juce::dontSendNotification);

    juce::Component::SafePointer<StemMaskingPanel> self(this);
    juce::String v = stemPaths_[0], d = stemPaths_[1], b = stemPaths_[2], o = stemPaths_[3];
    controller_.runInBackground([self, &work = controller_, v, d, b, o]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.analyzeStemMasking(v.toStdString(), d.toStdString(), b.toStdString(), o.toStdString());

        postIfCurrent(self, work, [result = std::move(result)](StemMaskingPanel& panel) {
            if (!result.is_ok()) {
                panel.statusLabel_.setText(TRANS("Analysis failed: ") + serviceErrorLine(result.error()),
                                           juce::dontSendNotification);
                return;
            }
            panel.statusLabel_.setText(TRANS("Analysis complete."), juce::dontSendNotification);

            // Sub-second time x frequency heatmap for the worst pair.
            const auto& hm = result.value().heatmap;
            panel.heatmapView_.grid = hm.grid;
            panel.heatmapView_.bandsHz.assign(hm.bands_hz.begin(), hm.bands_hz.end());
            panel.heatmapView_.blockMs = hm.block_ms;
            panel.heatmapView_.label =
                hm.grid.empty() ? juce::String()
                                : juce::String(hm.stem_a).toUpperCase() + juce::String(juce::CharPointer_UTF8(" x ")) +
                                      juce::String(hm.stem_b).toUpperCase() + " masking over time";
            panel.heatmapView_.repaint();

            for (const auto& pair : result.value().pairs) {
                for (auto& cell : panel.cells_) {
                    if (cell.stemA == juce::String(pair.stem_a) && cell.stemB == juce::String(pair.stem_b)) {
                        cell.score01 = pair.collision_score;
                        cell.windows = pair.collisions_over_time;
                        cell.repaint();
                        break;
                    }
                }
            }

            // Feed the collision scores into the copilot brain - previously
            // this detector was purely display-only, disconnected from
            // RecommendationEngine (2026-07-07 audit finding). Rebroadcasting
            // through AppController means every panel showing recommendations
            // (Dashboard, SuggestionListPanel) picks these up on next refresh.
            MaskingSummary summary;
            summary.bands_hz = toFloats(result.value().bands_hz);
            for (const auto& pair : result.value().pairs) {
                MaskingPairSummary ps;
                ps.stem_a = pair.stem_a;
                ps.stem_b = pair.stem_b;
                ps.collision_score = static_cast<float>(pair.collision_score);
                ps.band_scores = toFloats(pair.band_scores);
                summary.pairs.push_back(std::move(ps));
            }
            panel.controller_.analysis().setStemMaskingResult(summary);
        }, [](StemMaskingPanel& panel) { panel.analyzeButton_.setEnabled(true); });
    });
}

void StemMaskingPanel::selectPairForTimeStrip(const Cell& cell)
{
    if (cell.score01 < 0.0) return; // not yet analyzed
    timeStrip_.pairLabel = cell.stemA.toUpperCase() + " / " + cell.stemB.toUpperCase() + " over time";
    timeStrip_.windows = cell.windows;
    timeStrip_.totalDuration = cell.windows.empty() ? 0.0 : cell.windows.back().end_sec;
    timeStrip_.repaint();

    selectedStemA_ = cell.stemA;
    selectedStemB_ = cell.stemB;
    fixSuggestionLabel_.setText(suggestFix(cell.stemA, cell.stemB, cell.score01), juce::dontSendNotification);
    generateFixButton_.setEnabled(cell.score01 >= 0.33);
}

juce::String StemMaskingPanel::suggestFix(const juce::String& stemA, const juce::String& stemB, double score01)
{
    if (score01 < 0.33)
        return stemA.toUpperCase() + " / " + stemB.toUpperCase() + ": low collision risk, no fix needed.";

    // Ordered independent of click order - the pair identity is what
    // determines the fix, not which stem happened to be stemA vs stemB.
    juce::StringArray pair{stemA.toLowerCase(), stemB.toLowerCase()};
    pair.sort(false);
    const juce::String key = pair.joinIntoString("+");

    juce::String fix;
    if (key == "drums+vocals")
        fix = "Duck drums (especially snare/cymbals, 2-6 kHz) under the vocal via sidechain compression, "
              "or a 2-3 dB static dip in that band during vocal sections.";
    else if (key == "bass+vocals")
        fix = "High-pass the bass more aggressively (~100-150 Hz) to stop it crowding the vocal's low-mids.";
    else if (key == "other+vocals")
        fix = "Carve a narrow notch in 'other' around the vocal fundamental (roughly 200-500 Hz).";
    else if (key == "bass+drums")
        fix = "Sidechain-compress the bass from the kick, or high-pass the kick below ~40 Hz to leave "
              "room for the bass fundamental.";
    else if (key == "drums+other")
        fix = "EQ-cut 'other' around the kick/snare transient bands (150-250 Hz and 2-5 kHz).";
    else if (key == "bass+other")
        fix = "Split the low end at ~200 Hz: high-pass 'other' there, low-pass bass's upper harmonics.";
    else
        fix = "Try a complementary EQ cut in one stem where the other is strongest.";

    const juce::String severity = score01 >= 0.66 ? "High" : "Moderate";
    return severity + " collision (" + juce::String(juce::roundToInt(score01 * 100.0)) + "%): " + fix;
}

void StemMaskingPanel::generateFixChain()
{
    if (selectedStemA_.isEmpty() || selectedStemB_.isEmpty()) return;

    generateFixButton_.setEnabled(false);
    fixSuggestionLabel_.setText(fixSuggestionLabel_.getText() + "\n\nGenerating chain suggestion...",
                                juce::dontSendNotification);

    // Target the stem more likely to need a corrective cut - vocals take
    // priority over everything, otherwise fall back to stemA.
    juce::String target = (selectedStemA_.equalsIgnoreCase("vocals") || selectedStemB_.equalsIgnoreCase("vocals"))
                              ? juce::String("vocals")
                              : selectedStemA_;

    juce::String genre, mood = "modern", energy = "high";
    double bpm = 120.0;
    if (const auto* s = controller_.getActiveSession()) genre = juce::String(s->target.primary_genre);
    if (controller_.hasAnalysis()) bpm = controller_.currentAnalysis().python_data.bpm_detected;

    const juce::String styleDesc =
        "Corrective chain to resolve frequency masking between " + selectedStemA_ + " and " + selectedStemB_ + ".";

    juce::Component::SafePointer<StemMaskingPanel> self(this);
    controller_.runLongInBackground([self, &work = controller_, target, genre, mood, energy, bpm, styleDesc]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.generateMixerChain(target, genre, energy, mood, bpm, styleDesc, {});
        postIfCurrent(self, work, [result = std::move(result)](StemMaskingPanel& panel) {
            if (result.is_err()) {
                panel.fixSuggestionLabel_.setText(panel.fixSuggestionLabel_.getText() +
                                                      "\nFailed: " + juce::String(result.error().message),
                                                  juce::dontSendNotification);
                return;
            }
            juce::String text = panel.fixSuggestionLabel_.getText() + "\n\nSuggested chain:";
            for (const auto& p : result.value().plugins)
                text += "\n  - " + p.name + " (" + p.category + ") - " + p.settings_description;
            panel.fixSuggestionLabel_.setText(text, juce::dontSendNotification);
        }, [](StemMaskingPanel& panel) { panel.generateFixButton_.setEnabled(true); });
    });
}

void StemMaskingPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();

    auto topRow = b.removeFromTop(22);
    analyzeButton_.setBounds(topRow.removeFromLeft(140));
    topRow.removeFromLeft(8);
    statusLabel_.setBounds(topRow);
    b.removeFromTop(4);

    separationInfoLabel_.setBounds(b.removeFromTop(14));
    b.removeFromTop(2);
    audibilityLabel_.setBounds(b.removeFromTop(26));
    b.removeFromTop(4);

    auto fixRow = b.removeFromBottom(24);
    generateFixButton_.setBounds(fixRow.removeFromRight(140));
    b.removeFromBottom(4);

    auto fixLabelArea = b.removeFromBottom(48);
    fixSuggestionLabel_.setBounds(fixLabelArea);
    b.removeFromBottom(4);

    auto stripArea = b.removeFromBottom(32);
    b.removeFromBottom(4);
    timeStrip_.setBounds(stripArea);

    // Time x frequency masking heatmap, above the per-window strip.
    auto heatArea = b.removeFromBottom(72);
    b.removeFromBottom(4);
    heatmapView_.setBounds(heatArea);

    const int cellH = juce::jmax(30, b.getHeight() / (int)cells_.size());
    for (auto& cell : cells_) {
        cell.setBounds(b.removeFromTop(cellH));
    }
}

void StemMaskingPanel::MaskingHeatmapView::paint(juce::Graphics& g)
{
    auto area = getLocalBounds();
    auto titleRow = area.removeFromTop(12);
    g.setFont(khensu::KhensuTheme::uiFont(8.5f, juce::Font::bold));
    g.setColour(KhensuTheme::textSecondary);
    g.drawText(label.isEmpty() ? "Masking over time" : label, titleRow, juce::Justification::centredLeft, false);

    auto grid_area = area.reduced(0, 1);
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
    g.fillRect(grid_area);

    if (grid.empty() || bandsHz.empty()) {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::italic));
        g.drawText("Analyze masking to see the transient-scale heatmap.", grid_area, juce::Justification::centred,
                   false);
        return;
    }

    const int nFrames = (int)grid.size();
    const int nBands = (int)bandsHz.size();
    const float cw = (float)grid_area.getWidth() / (float)juce::jmax(1, nFrames);
    const float ch = (float)grid_area.getHeight() / (float)juce::jmax(1, nBands);

    for (int f = 0; f < nFrames; ++f) {
        const auto& col = grid[(size_t)f];
        for (int band = 0; band < nBands && band < (int)col.size(); ++band) {
            const float v = juce::jlimit(0.0f, 1.0f, col[(size_t)band]);
            if (v <= 0.01f) continue;
            // teal (cool) -> amber -> red (hot) collision ramp.
            const juce::Colour c =
                v < 0.5f ? KhensuTheme::accentTeal.interpolatedWith(KhensuTheme::accentAmber, v * 2.0f)
                         : KhensuTheme::accentAmber.interpolatedWith(KhensuTheme::accentRed, (v - 0.5f) * 2.0f);
            // Band 0 = lowest freq drawn at the bottom.
            const float y = (float)grid_area.getY() + (float)(nBands - 1 - band) * ch;
            g.setColour(c.withAlpha(0.25f + 0.7f * v));
            g.fillRect((float)grid_area.getX() + (float)f * cw, y, cw + 0.5f, ch + 0.5f);
        }
    }

    // Faint band gridlines so the frequency rows read.
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.25f));
    for (int band = 1; band < nBands; ++band)
        g.drawHorizontalLine(juce::roundToInt((float)grid_area.getY() + (float)band * ch), (float)grid_area.getX(),
                             (float)grid_area.getRight());
}

} // namespace khensu
