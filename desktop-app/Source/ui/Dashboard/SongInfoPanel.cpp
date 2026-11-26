// desktop-app/Source/ui/Dashboard/SongInfoPanel.cpp
#include "SongInfoPanel.h"
#include "../shared/PanelRunState.h"
#include "../shared/SessionEpoch.h"
#include <khensu/session/Session.h>
#include <khensu/targets/GenreDatabase.h>
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include "../shared/ServiceErrorHint.h"
#include "../shared/SessionEpoch.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/GenreConfirmation.h>
#include <cmath>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

namespace {
constexpr std::array<const char*, 5> kTrustMetricKeys{{
    "loudness_lufs", "true_peak_dbtp", "crest_factor_db", "bpm", "key",
}};
constexpr std::array<const char*, 5> kTrustMetricLabels{{
    "Loudness", "True peak", "Dynamics", "Tempo", "Key",
}};

BadgeStatus trustStatus(const std::string& qualityFlag)
{
    if (qualityFlag == "reliable") return BadgeStatus::Pass;
    if (qualityFlag == "tentative") return BadgeStatus::Warn;
    return BadgeStatus::Fail;
}
} // namespace

void SongFingerprintStrip::paint(juce::Graphics& g)
{
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText("SONG FINGERPRINT (3D)", getLocalBounds().withHeight(12), juce::Justification::centredLeft, false);

    if (!hasData_) return;

    constexpr float kDepth = 5.0f;
    auto b = getLocalBounds().withTrimmedTop(13).toFloat();
    b.removeFromRight(kDepth);
    b.removeFromTop(kDepth);
    if (b.getHeight() < 4.0f) return;

    struct Bar {
        const char* label;
        float value;
        juce::Colour colour;
    };
    const Bar bars[] = {
        // UX2-3: one accent - the bars are told apart by their captions.
        {"Energy", energy_, KhensuTheme::accentTeal},
        {"Hook", hook_, KhensuTheme::accentTeal},
        {"Memo", memorability_, KhensuTheme::accentTeal},
    };
    constexpr int n = 3;
    constexpr float spacing = 8.0f;
    const float barW = (b.getWidth() - spacing * (n - 1)) / (float)n;

    g.setFont(KhensuTheme::labelFont(type::caption));
    for (int i = 0; i < n; ++i) {
        const float t = juce::jlimit(0.0f, 1.0f, bars[i].value / 100.0f);
        const float bx = b.getX() + (float)i * (barW + spacing);
        const float bh = b.getHeight() * (0.15f + 0.75f * t);
        const float by = b.getBottom() - bh - 11.0f; // leave room for the caption below

        KhensuLookAndFeel::draw3DHeatBar(g, {bx, by, barW, bh}, bars[i].colour, kDepth);

        g.setColour(KhensuTheme::textTertiary);
        g.drawText(bars[i].label, juce::Rectangle<float>(bx, b.getBottom() - 10.0f, barW, 10.0f),
                   juce::Justification::centred, false);
    }
}

void GenreBlendStrip::paint(juce::Graphics& g)
{
    if (blend_.size() < 2) return;

    static const juce::Colour kSegColours[] = {
        KhensuTheme::accentTeal, // UX2-3: one accent, then neutral steps
        KhensuTheme::textSecondary,
        KhensuTheme::textTertiary,
    };

    auto full = getLocalBounds().toFloat();
    auto barArea = full.removeFromTop(6.0f);
    float totalRank = 0.0f;
    for (const auto& entry : blend_)
        totalRank += juce::jmax(0.0f, entry.rank_score);
    totalRank = juce::jmax(totalRank, 1.0e-6f);

    float x = barArea.getX();
    for (size_t i = 0; i < blend_.size(); ++i) {
        const float w = barArea.getWidth() * juce::jmax(0.0f, blend_[i].rank_score) / totalRank;
        g.setColour(kSegColours[i % 3].withAlpha(0.85f));
        g.fillRect(juce::Rectangle<float>(x, barArea.getY(), w, barArea.getHeight()));
        x += w;
    }

    full.removeFromTop(2.0f);
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.setColour(KhensuTheme::textTertiary);
    juce::String label;
    for (size_t i = 0; i < blend_.size() && i < 3; ++i) {
        if (i > 0) label << "  -  ";
        label << juce::String(blend_[i].genre) << " relative " << juce::roundToInt(blend_[i].rank_score * 100.0f)
              << "%";
    }
    g.drawText(label, full.toNearestInt(), juce::Justification::centredLeft, true);
}

// Every cached palette colour on this component, in one place (UI-01).
void SongInfoPanel::applyThemeColours()
{
    genreContextLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    fileSpecsLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    trustLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    musicLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

juce::String SongInfoPanel::formatMusicalIdentity(const juce::String& key, const juce::String& camelot,
                                                  const juce::String& relativeKey, const juce::String& parallelKey,
                                                  const juce::String& timeSignature, float meterConfidence,
                                                  float bpm, float tuningHz)
{
    juce::StringArray parts;
    if (key.isNotEmpty()) parts.add(key);
    if (camelot.isNotEmpty()) parts.add(camelot);
    if (relativeKey.isNotEmpty()) parts.add(TRANS("rel.") + " " + relativeKey);
    if (parallelKey.isNotEmpty()) parts.add(TRANS("par.") + " " + parallelKey);
    if (timeSignature.isNotEmpty())
        parts.add(meterConfidence >= 0.0f && meterConfidence <= 0.2f ? timeSignature + "?" : timeSignature);
    if (bpm > 0.0f)
        parts.add((bpm < 100.0f ? juce::String(bpm, 1) : juce::String(juce::roundToInt(bpm))) + " BPM");
    if (tuningHz > 0.0f) parts.add("A = " + juce::String(tuningHz, 1) + " Hz");
    return parts.joinIntoString(juce::String(juce::CharPointer_UTF8(" \xc2\xb7 ")));
}

void SongInfoPanel::setTuningReference(float referenceHz)
{
    tuningHz_ = referenceHz;
    refreshMusicLabel();
    resized();
}

void SongInfoPanel::refreshMusicLabel()
{
    const auto line = formatMusicalIdentity(music_.key, music_.camelot, music_.relativeKey, music_.parallelKey,
                                            music_.timeSignature, music_.meterConfidence, music_.bpm, tuningHz_);
    musicLabel_.setText(line.isEmpty() ? juce::String() : TRANS("Musical identity") + ":  " + line,
                        juce::dontSendNotification);
    const bool fallbackMeter = music_.timeSignature.isNotEmpty() && music_.meterConfidence >= 0.0f &&
                               music_.meterConfidence <= 0.2f;
    musicLabel_.setTooltip(
        fallbackMeter ? TRANS("Meter confidence is low - the time signature is a 4/4 fallback, not a detection.")
                      : TRANS("Key, Camelot code, relative / parallel keys, meter, tempo and tuning reference."));
}

SongInfoPanel::SongInfoPanel(IAppController& c) : PanelCard(c, "Song Info")
{
    applyThemeColours();
    setExplainer(TRANS("Song Info"), TRANS("General track metadata: detected genre/mood/energy, plus the file's raw specs "
                              "(sample rate, bit depth, channels, duration). The fingerprint strip is a quick "
                              "visual read of energy, hook strength, and memorability."));

    addAndMakeVisible(genreBadge_);
    addAndMakeVisible(moodBadge_);
    genreContextLabel_.setFont(KhensuTheme::labelFont(type::caption));
    addAndMakeVisible(genreContextLabel_);
    confirmGenreButton_.setTooltip(TRANS("Use the detected genre as this session's genre, so the Mastering Agent, "
                                         "Section Intensity and genre-relative chat chains can run."));
    confirmGenreButton_.onClick = [this] { confirmDetectedGenre(); };
    addChildComponent(confirmGenreButton_);
    addAndMakeVisible(genreBlend_);
    addAndMakeVisible(energyMeter_);
    energyMeter_.setRange(0.0f, 100.0f);
    energyMeter_.setTargetZone(40.0f, 80.0f);
    for (auto& ring : targetRings_) addChildComponent(ring);

    fileSpecsLabel_.setFont(KhensuTheme::bodyFont(type::caption));
    addAndMakeVisible(fileSpecsLabel_);
    musicLabel_.setFont(KhensuTheme::bodyFont(type::caption));
    musicLabel_.setMinimumHorizontalScale(0.7f);
    addAndMakeVisible(musicLabel_);
    addChildComponent(errorState_);
    errorState_.onRetry([this] { fetchMeasurementTrust(); });

    // Measurement trust map (pipeline #5): how much to trust the key numbers.
    trustLabel_.setFont(KhensuTheme::labelFont(type::caption));
    addChildComponent(trustLabel_);
    for (auto& badge : trustBadges_) addChildComponent(badge);

    addAndMakeVisible(fingerprint_);

    // File specs come straight from the probed AudioFileRef, not the
    // AnalysisResult - populate once at construction since they don't
    // change across re-analysis of the same session.
    if (const auto* s = controller_.getActiveSession(); s && !s->files.empty()) {
        const auto& f = s->files.front();
        const int durSec = (int)std::lround(f.duration_seconds);
        fileSpecsLabel_.setText(juce::String(f.sample_rate / 1000.0, 1) + " kHz  -  " + juce::String(f.bit_depth) +
                                    "-bit  -  " +
                                    (f.channels == 1   ? "Mono"
                                     : f.channels == 2 ? "Stereo"
                                                       : juce::String(f.channels) + "ch") +
                                    "  -  " + juce::String::formatted("%d:%02d", durSec / 60, durSec % 60),
                                juce::dontSendNotification);
    }
}

void SongInfoPanel::setData(const AnalysisResult& r)
{
    juce::String genre = juce::String(r.python_data.genre_detected);
    juce::String subgenre = juce::String(r.python_data.subgenre_detected);
    juce::String mood = juce::String(r.python_data.mood_detected);
    float energy = r.python_data.energy_detected;
    float hookStr = r.python_data.hooks.hook_strength * 100.0f;
    float memoScore = r.python_data.memorability.score * 100.0f;
    auto blend = r.genre_blend;

    // No analysis (an empty result, e.g. the visibility refresh before the first
    // result): an empty state. This used to invent a "Synthwave" demo song,
    // which read as a finding about the user's track.
    const bool noAnalysis = !r.has_fast_analysis();
    if (noAnalysis) {
        genre.clear();
        subgenre.clear();
        mood.clear();
        energy = 0.0f;
        hookStr = 0.0f;
        memoScore = 0.0f;
        blend.clear();
        fileSpecsLabel_.setText(TRANS("No analysis yet"), juce::dontSendNotification);
    }

    juce::String genreText = genre.isEmpty()      ? "Genre -"
                             : subgenre.isEmpty() ? "Genre: " + genre
                                                  : "Genre: " + genre + " / " + subgenre;

    // Live, not r.genre_relative_action_allowed: the deep response can't know
    // the session's confirmation, and a confirmation made here must show.
    const bool genreAllowed = !noAnalysis && controller_.analysis().genreRelativeActionAllowed();
    BadgeStatus genreStatus = BadgeStatus::Neutral;
    if (genre.isNotEmpty()) {
        // Genre detection is a candidate, not a calibrated reliability score.
        // Do not display the legacy numeric confidence as if it were one.
        genreStatus = !genreAllowed ? BadgeStatus::Warn : BadgeStatus::Pass;
    }
    genreBadge_.setBadge(genreText, genreStatus);
    genreBadge_.setAlpha(genreStatus == BadgeStatus::Warn ? 0.7f : 1.0f);

    genreBlend_.setBlend(blend);
    genreBlend_.setVisible(blend.size() >= 2);

    if (noAnalysis) {
        genreContextLabel_.setText(TRANS("No analysis yet"), juce::dontSendNotification);
    } else if (r.genre_is_hybrid) {
        genreContextLabel_.setText(TRANS("Hybrid / section-conflicted - choose a genre or reference"),
                                   juce::dontSendNotification);
    } else if (!genreAllowed) {
        genreContextLabel_.setText(TRANS("Candidate only - confirm genre or load a reference before targets"),
                                   juce::dontSendNotification);
        genreContextLabel_.setTooltip(controller_.analysis().genreRelativeAbstainedReason());
    } else {
        genreContextLabel_.setText(TRANS("Genre target confirmed by user/reference"), juce::dontSendNotification);
        genreContextLabel_.setTooltip({});
    }
    const auto* activeSession = controller_.getActiveSession();
    const bool canConfirm = !noAnalysis && !genreAllowed && !r.genre_is_hybrid && genre.isNotEmpty() &&
                            activeSession != nullptr && activeSession->target.use_genre_targets;
    if (confirmGenreButton_.isVisible() != canConfirm) {
        confirmGenreButton_.setVisible(canConfirm);
        resized();
    }

    moodBadge_.setBadge(mood.isEmpty() ? "Mood -" : "Mood: " + mood,
                        mood.isEmpty() ? BadgeStatus::Neutral : BadgeStatus::Pass);

    energyMeter_.setValue(energy, "Energy: " + juce::String((int)std::round(energy)));

    // Do not infer a genre target from the detector. These gauges appear only
    // when the active session has an explicit genre selection.
    std::optional<GenreEntry> targetGenre;
    if (!noAnalysis) {
        if (const auto* session = controller_.getActiveSession(); session != nullptr) {
            if (auto genreTarget = controller_.genreDatabase().findById(session->target.primary_genre))
                targetGenre = std::move(genreTarget);
        }
    }
    if (targetGenre.has_value()) {
        const float lufsTolerance = juce::jmax(0.5f, (targetGenre->lufs_range_max - targetGenre->lufs_range_min) * 0.5f);
        targetRings_[0].setTarget("LOUDNESS", r.loudness.lufs_integrated, targetGenre->lufs_target, lufsTolerance,
                                  " LUFS");
        targetRings_[1].setTarget("DYNAMICS", r.dynamics.crest_factor_db, targetGenre->crest_factor_target_db, 1.5f,
                                  " dB");
        targetRings_[2].setTarget("TILT", r.spectral.tilt_db_per_octave,
                                  targetGenre->spectral_tilt_target_db_per_octave, 0.75f, " dB/oct");
    }
    for (auto& ring : targetRings_) ring.setVisible(targetGenre.has_value());

    if (noAnalysis) {
        music_ = {};
    } else {
        music_.key = juce::String(r.python_data.pitch_key);
        music_.camelot = juce::String(r.python_data.camelot);
        music_.relativeKey = juce::String(r.python_data.relative_key);
        music_.parallelKey = juce::String(r.python_data.parallel_key);
        // The chord analyzer reports the same relations for its own key
        // estimate - use them when the deep pass left them empty.
        if (r.chord_analysis.has_value()) {
            const auto& ca = *r.chord_analysis;
            if (music_.key.isEmpty() && !ca.key.empty()) music_.key = juce::String(ca.key + " " + ca.mode).trim();
            if (music_.camelot.isEmpty()) music_.camelot = juce::String(ca.camelot);
            if (music_.relativeKey.isEmpty()) music_.relativeKey = juce::String(ca.relative_key);
            if (music_.parallelKey.isEmpty()) music_.parallelKey = juce::String(ca.parallel_key);
        }
        music_.timeSignature = juce::String(r.python_data.time_signature);
        music_.meterConfidence = r.python_data.meter_confidence;
        music_.bpm = r.python_data.bpm_detected;
    }
    refreshMusicLabel();

    fingerprint_.setValues(energy, hookStr, memoScore);
    // A new/partial analysis must not retain the previous session's trust
    // summary while the best-effort refresh below is still in flight.
    if (r.measurement_trust.empty())
        trustLabel_.setText({}, juce::dontSendNotification);
    setMeasurementTrust(r.measurement_trust);
    repaint();

    // Measurement trust map (pipeline #5): fetch per-measurement confidence and
    // surface a compact summary ("4/5 reliable - bpm tentative"). Best-effort;
    // hidden until it returns. Skipped when there is no analysis.
    errorState_.setVisible(false);
    resized();
    if (noAnalysis) return;
    fetchMeasurementTrust();
}

void SongInfoPanel::fetchMeasurementTrust()
{
    const juce::String target = controller_.activeSourcePath();
    if (target.isEmpty()) return;
    errorState_.setVisible(false);
    juce::Component::SafePointer<SongInfoPanel> self(this);
    controller_.runInBackground([self, target, &work = controller_]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.analyzeMeasurementQuality(target.toStdString());
        postIfCurrent(self, work, [result, target](SongInfoPanel& panel) {
            if (panel.controller_.activeSourcePath() != target) return;
            if (result.is_err()) {
                const auto hint = classifyServiceError(result.error());
                panel.errorState_.setError(TRANS("Measurement trust unavailable"), hint.message + "  " + hint.remedy);
                panel.errorState_.setVisible(true);
                panel.resized();
                return;
            }
            panel.errorState_.setVisible(false);
            const juce::var v = result.value();
            const int reliable = (int)v[af::MeasurementQualityResponse::n_reliable];
            const int total = (int)v[af::MeasurementQualityResponse::n_total];
            if (total <= 0) return;
            juce::StringArray shaky;
            if (auto* ms = v[af::MeasurementQualityResponse::measurements].getDynamicObject()) {
                for (const auto& prop : ms->getProperties())
                    if (prop.value[af::QualifiedMeasurement::quality_flag].toString() != "reliable")
                        shaky.add(prop.name.toString());
            }
            juce::String txt = TRANS("Measurement trust:") + " " + juce::String(reliable) + "/" + juce::String(total)
                               + " " + TRANS("reliable");
            if (!shaky.isEmpty())
                txt += juce::String(juce::CharPointer_UTF8("  \xc2\xb7  ")) + shaky.joinIntoString(", ") + " "
                       + TRANS("shaky");
            panel.trustLabel_.setText(txt, juce::dontSendNotification);
            panel.trustLabel_.setVisible(true);
            std::map<std::string, AnalysisResult::MeasurementTrust> trust;
            if (auto* ms = v[af::MeasurementQualityResponse::measurements].getDynamicObject()) {
                for (const auto& prop : ms->getProperties()) {
                    AnalysisResult::MeasurementTrust item;
                    item.confidence = static_cast<float>(static_cast<double>(prop.value[af::QualifiedMeasurement::confidence]));
                    item.quality_flag = prop.value[af::QualifiedMeasurement::quality_flag].toString().toStdString();
                    item.note = prop.value[af::QualifiedMeasurement::note].toString().toStdString();
                    trust[prop.name.toString().toStdString()] = std::move(item);
                }
            }
            panel.setMeasurementTrust(trust);
            panel.resized();
        });
    });
}

void SongInfoPanel::setMeasurementTrust(const std::map<std::string, AnalysisResult::MeasurementTrust>& trust)
{
    bool anyVisible = false;
    for (size_t i = 0; i < trustBadges_.size(); ++i) {
        const auto it = trust.find(kTrustMetricKeys[i]);
        if (it == trust.end()) {
            trustBadges_[i].setVisible(false);
            continue;
        }
        const auto& item = it->second;
        juce::String text(kTrustMetricLabels[i]);
        text += " " + juce::String(static_cast<int>(std::round(item.confidence * 100.0f))) + "%";
        if (item.quality_flag != "reliable") text += " " + juce::String(item.quality_flag);
        trustBadges_[i].setBadge(text, trustStatus(item.quality_flag));
        trustBadges_[i].setVisible(true);
        anyVisible = true;
    }
    trustLabel_.setVisible(anyVisible || trustLabel_.getText().isNotEmpty());
}

void SongInfoPanel::confirmDetectedGenre()
{
    const auto* session = controller_.getActiveSession();
    if (session == nullptr || !controller_.hasAnalysis()) return;
    auto target = session->target;
    target.genre_confirmed_source = genre_confirmation::kConfirmedCandidate;
    // Adopt the detected genre as the session's genre when it has none and
    // the detection names a genre the database knows.
    const auto& detected = controller_.currentAnalysis().python_data.genre_detected;
    if (target.primary_genre.empty() && controller_.genreDatabase().findById(detected).has_value())
        target.primary_genre = detected;
    if (!controller_.sessions().updateActiveSessionTarget(target)) return;
    setData(controller_.currentAnalysis());
}

void SongInfoPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();

    auto row = b.removeFromTop(26);
    genreBadge_.setBounds(row.removeFromLeft(row.getWidth() / 2 - 4));
    row.removeFromLeft(8);
    moodBadge_.setBounds(row);
    b.removeFromTop(4);

    if (genreBlend_.isVisible()) {
        genreBlend_.setBounds(b.removeFromTop(18));
        b.removeFromTop(4);
    }

    auto contextRow = b.removeFromTop(20);
    if (confirmGenreButton_.isVisible()) {
        confirmGenreButton_.setBounds(contextRow.removeFromRight(110).reduced(0, 1));
        contextRow.removeFromRight(6);
    }
    genreContextLabel_.setBounds(contextRow);
    b.removeFromTop(2);

    energyMeter_.setBounds(b.removeFromTop(34));
    b.removeFromTop(6);

    if (targetRings_[0].isVisible()) {
        auto ringRow = b.removeFromTop(62);
        const int gap = 4;
        const int ringWidth = (ringRow.getWidth() - gap * 2) / 3;
        for (size_t i = 0; i < targetRings_.size(); ++i)
            targetRings_[i].setBounds(ringRow.getX() + (int)i * (ringWidth + gap), ringRow.getY(), ringWidth,
                                      ringRow.getHeight());
        b.removeFromTop(4);
    }

    fileSpecsLabel_.setBounds(b.removeFromTop(16));
    b.removeFromTop(2);
    if (musicLabel_.getText().isNotEmpty()) {
        musicLabel_.setBounds(b.removeFromTop(16));
        b.removeFromTop(4);
    } else {
        musicLabel_.setBounds({});
    }

    if (trustLabel_.isVisible()) {
        trustLabel_.setBounds(b.removeFromTop(14));
        b.removeFromTop(4);
    }

    int visibleIndex = 0;
    for (auto& badge : trustBadges_) {
        if (!badge.isVisible()) continue;
        const int col = visibleIndex % 2;
        const int rowIndex = visibleIndex / 2;
        const int width = b.getWidth() / 2 - 2;
        badge.setBounds(b.getX() + col * (width + 4), b.getY() + rowIndex * 22, width, 18);
        ++visibleIndex;
    }
    if (visibleIndex > 0) b.removeFromTop(((visibleIndex + 1) / 2) * 22 + 2);

    if (errorState_.isVisible()) {
        errorState_.setBounds(b.withHeight(juce::jlimit(0, 140, b.getHeight())));
        fingerprint_.setBounds({});
    } else {
        fingerprint_.setBounds(b);
    }
}

} // namespace khensu
