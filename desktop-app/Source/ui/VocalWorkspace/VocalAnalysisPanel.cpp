// desktop-app/Source/ui/VocalWorkspace/VocalAnalysisPanel.cpp
#include "VocalAnalysisPanel.h"
#include "../shared/PanelRunState.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include "../../IAppController.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

// Every cached palette colour on this component, in one place (UI-01).
void VocalAnalysisPanel::applyThemeColours()
{
    applyDeessButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.15f));
    applyDeessButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
}

VocalAnalysisPanel::VocalAnalysisPanel(IAppController& c)
    : PanelCard(c, "Vocal Analysis \u2014 Master Mix Estimate"), controller_(c)
{
    applyThemeColours();
    startTimer(100);
    setExplainer(TRANS("Vocal Analysis"), TRANS("WARNING: These values are master-mix heuristics, NOT vocal stem measurements.\n\n"
                                   "Vocal Presence = mid energy of the FULL MIX (250Hz-4kHz). Guitars, keys, and "
                                   "other mid-range instruments inflate this value.\n\n"
                                   "Sibilance Risk = high-frequency energy of the FULL MIX (>4kHz). Cymbals and "
                                   "hi-hats inflate this value.\n\n"
                                   "Use Stem Separation first for accurate vocal measurements."));

    presenceMeter_.setRange(0.0f, 1.0f);
    presenceMeter_.setTargetZone(0.25f, 0.55f);
    sibilanceMeter_.setRange(0.0f, 1.0f);
    sibilanceMeter_.setTargetZone(0.10f, 0.30f);
    dynamicsMeter_.setRange(0.0f, 30.0f);
    dynamicsMeter_.setTargetZone(6.0f, 18.0f);

    addAndMakeVisible(presenceMeter_);
    addAndMakeVisible(sibilanceMeter_);
    addAndMakeVisible(dynamicsMeter_);
    addAndMakeVisible(levelBadge_);

    applyDeessButton_.setTooltip(TRANS("Sets the De-essing macro from the measured sibilance: the "
                                 "peak-decile sibilance level maps to de-esser strength, and the "
                                 "measured dominant frequency is already used by the render chain."));
    applyDeessButton_.setEnabled(false);
    applyDeessButton_.onClick = [this] { applyDeessRecommendation(); };
    addAndMakeVisible(applyDeessButton_);
}

void VocalAnalysisPanel::applyDeessRecommendation()
{
    if (sibTimeline_.empty()) return;
    // Strength from the loud tail of the distribution: de-essers act on the
    // worst moments, not the average.
    std::vector<float> sorted = sibTimeline_;
    std::sort(sorted.begin(), sorted.end());
    const size_t idx = sorted.size() - juce::jmax((size_t)1, sorted.size() / 10);
    float peakDecile = 0.0f;
    for (size_t i = idx; i < sorted.size(); ++i)
        peakDecile += sorted[i];
    peakDecile /= (float)(sorted.size() - idx);

    // 0.2 sibilance -> gentle 35; 0.8+ -> aggressive 90.
    const float macro = juce::jlimit(0.0f, 100.0f, 35.0f + (peakDecile - 0.2f) / 0.6f * 55.0f);
    controller_.vocalEdit().setVocalMacroValue("De-essing", macro);
    applyDeessButton_.setButtonText(TRANS("De-esser set to ") + juce::String((int)macro) +
                                    (sibilanceHz_ > 0 ? " @ " + juce::String(sibilanceHz_ / 1000.0f, 1) + " kHz" : ""));
}

void VocalAnalysisPanel::setStemsAvailable(bool available)
{
    stemsAvailable_ = available;
    // Prefer the isolated vocal stem for the time-resolved analysis when it
    // exists; otherwise fall back to the master mix.
    refreshVocalTimeline();
    repaint();
}

void VocalAnalysisPanel::refreshVocalTimeline()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || s->files.empty()) return;

    juce::String currentSolo = controller_.playback().currentStemSolo();
    juce::File targetFile =
        currentSolo.isEmpty() ? juce::File(juce::String(s->files[0].path)) : controller_.getStemFile(currentSolo);

    if (!targetFile.existsAsFile()) return;
    const juce::String target = targetFile.getFullPathName();

    struct Fetched {
        std::vector<float> sib;
        std::vector<float> sii;
        std::vector<std::pair<float, float>> breaths;
        float presence = 0.0f, clarity = 0.0f, warmth = 0.0f, window = 0.0f, sibHz = 0.0f, siiScore = 1.0f;
    };
    fetchQuietly(*this, controller_, [target]() -> std::optional<Fetched> {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.analyzeVocal(target.toStdString());
        if (result.is_err()) return std::nullopt;
        const juce::var v = result.value();

        std::vector<float> sib;
        if (auto* arr = v[af::VocalAnalysisResponse::sibilance_timeline].getArray())
            for (const auto& e : *arr)
                sib.push_back((float)(double)e[af::SibilanceSample::sibilance]);
        std::vector<float> sii;
        if (auto* arr = v[af::VocalAnalysisResponse::sii_timeline].getArray())
            for (const auto& e : *arr)
                sii.push_back((float)(double)e[af::SiiSample::sii]);
        std::vector<std::pair<float, float>> breaths;
        if (auto* arr = v[af::VocalAnalysisResponse::breath_markers].getArray())
            for (const auto& e : *arr)
                breaths.emplace_back((float)(double)e[af::BreathMarker::time_sec], (float)(double)e[af::BreathMarker::duration_sec]);
        const float presence = (float)(double)v[af::VocalAnalysisResponse::presence_score];
        const float clarity = (float)(double)v[af::VocalAnalysisResponse::clarity_score];
        const float warmth = (float)(double)v[af::VocalAnalysisResponse::warmth_score];
        const float window = (float)(double)v[af::VocalAnalysisResponse::window_sec];
        const float sibHz = (float)(double)v[af::VocalAnalysisResponse::dominant_sibilance_hz];
        const float siiScore = (v.getDynamicObject() != nullptr && v.getDynamicObject()->hasProperty(af::VocalAnalysisResponse::vocal_sii_score))
                                   ? (float)(double)v[af::VocalAnalysisResponse::vocal_sii_score]
                                   : 1.0f;

        return Fetched{std::move(sib), std::move(sii), std::move(breaths), presence, clarity, warmth, window, sibHz, siiScore};
    }, [](VocalAnalysisPanel& self, std::optional<Fetched>&& fetched) {
        if (!fetched) return;
        auto& [sib, sii, breaths, presence, clarity, warmth, window, sibHz, siiScore] = *fetched;
        self.sibTimeline_ = std::move(sib);
        self.siiTimeline_ = std::move(sii);
        self.breathMarkers_ = std::move(breaths);
        self.presenceScore_ = presence;
        self.clarityScore_ = clarity;
        self.warmthScore_ = warmth;
        self.sibilanceHz_ = sibHz;
        self.siiScore_ = siiScore;
        self.totalDurationSec_ = window * (float)self.sibTimeline_.size();
        self.applyDeessButton_.setEnabled(!self.sibTimeline_.empty());
        self.repaint();
    });
}

void VocalAnalysisPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!stemsAvailable_) {
        drawEmptyState(g, TRANS("Vocal-specific metrics require stem isolation."), TRANS("Run isolation first.")); // UXR-6
        return;
    }
    // ---- Sibilance heatmap over time ----
    if (!sibTimeline_.empty() && !heatmapArea_.isEmpty()) {
        auto hm = heatmapArea_;
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.setColour(KhensuTheme::textTertiary);
        juce::String header = "SIBILANCE OVER TIME (CLICK/DRAG TO SCRUB)";
        if (sibilanceHz_ > 0.0f)
            header << "  -  De-esser target: " << juce::String(sibilanceHz_ / 1000.0f, 1) << " kHz";
        g.drawText(header, hm.removeFromTop(12), juce::Justification::centredLeft, false);
        auto strip = hm.toFloat();
        const int n = (int)sibTimeline_.size();
        const float bw = strip.getWidth() / (float)n;
        for (int i = 0; i < n; ++i) {
            const float s = juce::jlimit(0.0f, 1.0f, sibTimeline_[(size_t)i]);
            // teal (calm) -> amber -> red (harsh)
            auto col = s < 0.5f
                           ? KhensuTheme::accentTeal.interpolatedWith(KhensuTheme::accentAmber, s * 2.0f)
                           : KhensuTheme::accentAmber.interpolatedWith(KhensuTheme::accentRed, (s - 0.5f) * 2.0f);
            g.setColour(col.withAlpha(0.35f + 0.65f * s));
            g.fillRect(strip.getX() + i * bw, strip.getY(), std::ceil(bw), strip.getHeight());
        }
        // Breath markers as ticks along the bottom.
        if (totalDurationSec_ > 0.0f) {
            g.setColour(KhensuTheme::textSecondary.withAlpha(0.8f));
            for (const auto& [start, dur] : breathMarkers_) {
                const float x = strip.getX() + strip.getWidth() * (start / totalDurationSec_);
                g.fillRect(x, strip.getBottom() - 4.0f, 2.0f, 4.0f);
            }
        }
        // Draw the playhead needle
        if (playheadSec_ >= 0.0f && totalDurationSec_ > 0.0f) {
            const float px = strip.getX() + strip.getWidth() * (playheadSec_ / totalDurationSec_);
            if (px >= strip.getX() && px <= strip.getRight()) {
                g.setColour(khensu::KhensuTheme::textPrimary);
                g.drawVerticalLine(static_cast<int>(px), strip.getY(), strip.getBottom());

                // Draw a small playhead triangle handle at the top
                juce::Path path;
                path.addTriangle(px - 4.0f, strip.getY(), px + 4.0f, strip.getY(), px, strip.getY() + 4.0f);
                g.fillPath(path);
            }
        }
    }

    // ---- Speech Intelligibility Index (SII) heatmap over time ----
    if (!siiTimeline_.empty() && !siiTimelineArea_.isEmpty()) {
        auto hm = siiTimelineArea_;
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText("SPEECH INTELLIGIBILITY (SII) OVER TIME (VS BACKING TRACK)", hm.removeFromTop(12),
                   juce::Justification::centredLeft, false);
        auto strip = hm.toFloat();
        const int n = (int)siiTimeline_.size();
        const float bw = strip.getWidth() / (float)n;
        for (int i = 0; i < n; ++i) {
            const float s = juce::jlimit(0.0f, 1.0f, siiTimeline_[(size_t)i]);
            // red (masked/poor) -> amber -> green/cyan (highly intelligible)
            auto col = s < 0.5f ? KhensuTheme::accentRed.interpolatedWith(KhensuTheme::accentAmber, s * 2.0f)
                                : KhensuTheme::accentAmber.interpolatedWith(KhensuTheme::accentTeal, (s - 0.5f) * 2.0f);
            g.setColour(col.withAlpha(0.25f + 0.75f * s));
            g.fillRect(strip.getX() + i * bw, strip.getY(), std::ceil(bw), strip.getHeight());
        }
        // Draw the playhead needle
        if (playheadSec_ >= 0.0f && totalDurationSec_ > 0.0f) {
            const float px = strip.getX() + strip.getWidth() * (playheadSec_ / totalDurationSec_);
            if (px >= strip.getX() && px <= strip.getRight()) {
                g.setColour(khensu::KhensuTheme::textPrimary);
                g.drawVerticalLine(static_cast<int>(px), strip.getY(), strip.getBottom());
            }
        }
    }

    // ---- Vocal balance tiles (presence / clarity / warmth / intelligibility) ----
    if (!tilesArea_.isEmpty() && (presenceScore_ + clarityScore_ + warmthScore_) > 0.0f) {
        struct Tile {
            const char* name;
            float v;
            juce::Colour col;
        };
        const Tile tiles[] = {
            {"PRESENCE", presenceScore_, KhensuTheme::accentTeal},
            {"CLARITY", clarityScore_, KhensuTheme::accentTeal},
            {"WARMTH", warmthScore_, KhensuTheme::accentTeal},
            {"INTELLIGIBILITY", siiScore_, KhensuTheme::accentBlue},
        };
        auto area = tilesArea_;
        const int tw = area.getWidth() / 4;
        for (const auto& t : tiles) {
            auto cell = area.removeFromLeft(tw).reduced(3);
            g.setColour(KhensuTheme::bgRaised);
            g.fillRoundedRectangle(cell.toFloat(), 4.0f);
            g.setColour(t.col.withAlpha(0.25f + 0.6f * t.v));
            g.fillRoundedRectangle(cell.toFloat()
                                       .withHeight(cell.getHeight() * juce::jlimit(0.05f, 1.0f, t.v))
                                       .withBottomY((float)cell.getBottom()),
                                   4.0f);
            g.setColour(KhensuTheme::textPrimary);
            g.setFont(KhensuTheme::metricFont(type::subtitle));
            g.drawText(juce::String(int(t.v * 100.0f)) + "%", cell.removeFromTop(cell.getHeight() - 14),
                       juce::Justification::centred, false);
            g.setFont(KhensuTheme::labelFont(type::caption));
            g.setColour(KhensuTheme::textTertiary);
            g.drawText(t.name, cell, juce::Justification::centred, false);
        }
    }
}

void VocalAnalysisPanel::setData(const AnalysisResult& r)
{
    const auto* s = controller_.getActiveSession();
    if (s != nullptr && !s->files.empty()) {
        juce::String currentSolo = controller_.playback().currentStemSolo();
        juce::File targetFile =
            currentSolo.isEmpty() ? juce::File(juce::String(s->files[0].path)) : controller_.getStemFile(currentSolo);
        juce::String targetPath = targetFile.getFullPathName();
        if (targetPath != lastAnalyzedTarget_) {
            lastAnalyzedTarget_ = targetPath;
            sibTimeline_.clear();
            refreshVocalTimeline();
        }
    }
    if (!stemsAvailable_) {
        repaint();
        return;
    }
    // controller_.stems().hasVocalAnalysis() means r is the isolated vocal stem's own
    // analysis (see VocalWorkspaceScreen::pushDataToPanels) -- this used to
    // always print "(master mix)" regardless, so the label lied about the
    // data source the moment ensureVocalAnalysis()/soloStem() populated the
    // real per-stem numbers. Same source-of-truth the LUFS badge below
    // already used correctly.
    const bool isVocalSource = controller_.stems().hasVocalAnalysis();
    const juce::String sourceTag = isVocalSource ? " (vocal)" : " (master mix)";
    presenceMeter_.setValue(r.spectral.balance_mid,
                            "Presence (est.): " + juce::String(juce::roundToInt(r.spectral.balance_mid * 100.0f)) + "% mid energy" +
                                sourceTag);
    sibilanceMeter_.setValue(r.spectral.balance_high,
                             "Sibilance Risk (est.): " + juce::String(juce::roundToInt(r.spectral.balance_high * 100.0f)) +
                                 "% high energy" + sourceTag);
    dynamicsMeter_.setValue(r.dynamics.crest_factor_db,
                            "Dynamics: " + juce::String(r.dynamics.crest_factor_db, 1) + " dB crest");

    const float lufs = r.loudness.lufs_integrated;
    const bool ok = lufs > -18.0f && lufs < -9.0f;
    levelBadge_.setBadge("LUFS " + juce::String(lufs, 1) + (stemsAvailable_ ? " (vocal)" : " (master)"),
                         ok ? BadgeStatus::Pass : BadgeStatus::Warn);
    repaint();
}

void VocalAnalysisPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    presenceMeter_.setBounds(b.removeFromTop(34));
    b.removeFromTop(6);
    sibilanceMeter_.setBounds(b.removeFromTop(34));
    b.removeFromTop(6);
    dynamicsMeter_.setBounds(b.removeFromTop(34));
    b.removeFromTop(8);
    levelBadge_.setBounds(b.removeFromTop(26));
    b.removeFromTop(8);

    // Time-resolved vocal visuals (painted directly).
    heatmapArea_ = b.removeFromTop(32);
    b.removeFromTop(6);
    applyDeessButton_.setBounds(b.removeFromTop(22).removeFromLeft(220));
    b.removeFromTop(6);
    siiTimelineArea_ = b.removeFromTop(32);
    b.removeFromTop(6);
    tilesArea_ = b.removeFromTop(juce::jmin(56, b.getHeight()));
}

void VocalAnalysisPanel::mouseDown(const juce::MouseEvent& e)
{
    if (totalDurationSec_ > 0.0f && heatmapArea_.contains(e.getPosition())) {
        scrubToMouse(e.x);
    }
}

void VocalAnalysisPanel::mouseDrag(const juce::MouseEvent& e)
{
    if (totalDurationSec_ > 0.0f && heatmapArea_.contains(e.x, heatmapArea_.getY())) {
        scrubToMouse(e.x);
    }
}

void VocalAnalysisPanel::timerCallback()
{
    if (totalDurationSec_ > 0.0f) {
        float pos = static_cast<float>(controller_.playback().getPlaybackPosition());
        if (std::abs(playheadSec_ - pos) > 0.01f) {
            playheadSec_ = pos;
            repaint();
        }
    }
}

void VocalAnalysisPanel::scrubToMouse(int mouseX)
{
    float pct = (mouseX - heatmapArea_.getX()) / (float)heatmapArea_.getWidth();
    pct = juce::jlimit(0.0f, 1.0f, pct);
    const double targetSec = pct * totalDurationSec_;
    controller_.playback().setPlaybackPosition(targetSec);
    repaint();
}

} // namespace khensu
