// desktop-app/Source/ui/VocalWorkspace/VocalQuickStatsPanel.cpp
#include "VocalQuickStatsPanel.h"
#include "../shared/PanelRunState.h"
#include "../shared/SessionEpoch.h"
#include <khensu/session/Session.h>
#include "../shared/KhensuTheme.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/MusicalContextText.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

VocalQuickStatsPanel::VocalQuickStatsPanel(IAppController& c) : PanelCard(c, "Track Quick Stats")
{
    setExplainer(TRANS("Quick Stats"), "Tempo, key, genre, and spectral balance from the fast analysis, plus "
                                "the format/level facts from the quick-stats service scan: duration, "
                                "sample rate, bit depth, integrated LUFS, loudness range, true peak "
                                "(4x oversampled dBTP), crest factor, stereo width, and a clipping "
                                "census (runs of >= 2 consecutive full-scale samples).");
}

void VocalQuickStatsPanel::setData(const AnalysisResult& r)
{
    if (!r.has_fast_analysis()) {
        // AUDIT-049: no analysis means an empty state - a hard-coded demo
        // "124 BPM, C# Minor, Synthwave" used to render here as if measured.
        bpm_ = 0.0f;
        key_.clear();
        genre_.clear();
        bpmDetail_.clear();
        keyDetail_.clear();
        balanceLow_ = balanceMid_ = balanceHigh_ = 0.0f;
        hasData_ = false;
        repaint();
        return;
    }

    const auto& pd = r.python_data;
    bpm_ = pd.bpm_detected;
    key_ = juce::String(pd.pitch_key);
    genre_ = juce::String(pd.genre_detected);
    balanceLow_ = r.spectral.balance_low;
    balanceMid_ = r.spectral.balance_mid;
    balanceHigh_ = r.spectral.balance_high;
    hasData_ = true;
    if (genre_.isEmpty()) genre_ = juce::String(L"-");
    if (key_.isEmpty()) key_ = juce::String(L"-");

    // Meter / felt tempo / Camelot / confidences (AUDIT-052) - only what the
    // service actually returned.
    bpmDetail_ = musical_context::tempoDetail(pd);
    keyDetail_ = musical_context::keyDetail(pd);

    // The level/format numbers come from the quick-stats service aggregate -
    // the fast analysis payload never carried them, which is why this card
    // used to show only BPM/Key/Genre (spec §2.2).
    fetchServiceStats();
    repaint();
}

void VocalQuickStatsPanel::fetchServiceStats()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || s->files.empty() || statsFetchInFlight_) return;
    const juce::String path(s->files[0].path);
    if (path == statsFetchedPath_) return; // already fetched (or fetching) for this file

    statsFetchInFlight_ = true;
    fetchQuietly(*this, controller_, [path]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.analyzeQuickStats(path.toStdString());
        return result;
    }, [path](VocalQuickStatsPanel& self, auto&& result) {
        if (result.is_err()) return; // fast row still shows; grid stays hidden until a retry
        self.statsFetchedPath_ = path;

        const juce::var v = result.value();
        const double dur = (double)v[af::QuickStatsResponse::duration_sec];
        const int min = (int)dur / 60, sec = (int)dur % 60;

        // Silence / sub-400 ms audio yields null LRA and crest.
        auto f1 = [](const juce::var& x) { return x.isVoid() ? juce::String("n/a") : juce::String((double)x, 1); };
        self.stats_[0] = {"LENGTH", juce::String(min) + ":" + juce::String(sec).paddedLeft('0', 2)};
        self.stats_[1] = {"FORMAT", juce::String((int)(double)v[af::QuickStatsResponse::sample_rate_hz] / 1000.0, 1) + "k / " +
                                         v[af::QuickStatsResponse::bit_depth].toString() + " / " + juce::String((int)v[af::QuickStatsResponse::channels]) +
                                         "ch"};
        // quick_stats reports silence as the -70 LUFS absolute gate, not null.
        const juce::var lufs = v[af::QuickStatsResponse::integrated_lufs];
        self.stats_[2] = {"LUFS", !lufs.isVoid() && (double)lufs <= -70.0 ? juce::String("n/a") : f1(lufs)};
        self.stats_[3] = {"LRA", f1(v[af::QuickStatsResponse::loudness_range_lu]) + " LU"};
        self.stats_[4] = {"TRUE PK", f1(v[af::QuickStatsResponse::true_peak_dbtp]) + " dBTP"};
        self.stats_[5] = {"CREST", f1(v[af::QuickStatsResponse::crest_factor_db]) + " dB"};
        self.stats_[6] = {"WIDTH", f1(v[af::QuickStatsResponse::stereo_width_pct]) + "%"};
        self.stats_[7] = {"DC", juce::String((double)v[af::QuickStatsResponse::dc_offset], 4)};
        self.clippingCount_ = (int)v[af::QuickStatsResponse::clipping_event_count];
        self.stats_[8] = {"CLIPS", juce::String(self.clippingCount_)};

        self.hasServiceStats_ = true;
        self.repaint();
    }, [](VocalQuickStatsPanel& self) { self.statsFetchInFlight_ = false; });
}

void VocalQuickStatsPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    auto b = bodyArea().toFloat();
    if (!hasData_) {
        drawEmptyState(g, "No vocal stats yet", "Runs once the vocal is analyzed.");
        return;
    }

    const float rowH = 36.0f;

    // Split into 3 columns
    const float colW = b.getWidth() / 3.0f;
    auto bpmRect = b.removeFromLeft(colW);
    auto keyRect = b.removeFromLeft(colW);
    auto genreRect = b; // remaining space

    // 1. BPM Column
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText("BPM", bpmRect.withHeight(12.0f), juce::Justification::centredLeft, false);

    g.setFont(KhensuTheme::metricFont(type::title));
    g.setColour(KhensuTheme::textPrimary);
    g.drawText(bpm_ > 0.0f ? juce::String(bpm_, 1) : juce::String(L"\x2013"),
               bpmRect.withTrimmedTop(14.0f).withHeight(22.0f), juce::Justification::centredLeft, false);
    if (bpmDetail_.isNotEmpty()) {
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(bpmDetail_, bpmRect.withTrimmedLeft(30.0f).withHeight(12.0f), juce::Justification::centredLeft,
                   true);
    }

    // 2. Key Column
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText("KEY", keyRect.withHeight(12.0f), juce::Justification::centredLeft, false);

    g.setFont(KhensuTheme::bodyFont(type::body));
    g.setColour(KhensuTheme::accentTeal);
    g.drawText(key_, keyRect.withTrimmedTop(14.0f).withHeight(22.0f), juce::Justification::centredLeft, false);
    if (keyDetail_.isNotEmpty()) {
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(keyDetail_, keyRect.withTrimmedLeft(30.0f).withHeight(12.0f), juce::Justification::centredLeft,
                   true);
    }

    // 3. Genre Column
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText("GENRE", genreRect.withHeight(12.0f), juce::Justification::centredLeft, false);

    g.setFont(KhensuTheme::bodyFont(type::caption));
    g.setColour(KhensuTheme::textSecondary);
    g.drawText(genre_, genreRect.withTrimmedTop(14.0f).withHeight(22.0f), juce::Justification::centredLeft, false);

    b.removeFromTop(rowH);

    // ── Service quick-stats grid (spec §2.2) ─────────────────────────────
    if (!gridArea_.isEmpty()) {
        auto ga = gridArea_.toFloat();
        if (hasServiceStats_) {
            constexpr int kCols = 3;
            const int rows = ((int)stats_.size() + kCols - 1) / kCols;
            const float cw = ga.getWidth() / (float)kCols;
            const float ch = ga.getHeight() / (float)rows;
            for (size_t i = 0; i < stats_.size(); ++i) {
                const auto cell = juce::Rectangle<float>(ga.getX() + cw * (float)((int)i % kCols),
                                                         ga.getY() + ch * (float)((int)i / kCols), cw, ch);
                g.setFont(KhensuTheme::labelFont(type::caption));
                g.setColour(KhensuTheme::textTertiary);
                g.drawText(stats_[i].label, cell.withHeight(11.0f), juce::Justification::centredLeft, false);
                g.setFont(KhensuTheme::monoFont(type::caption));
                const bool isClips = stats_[i].label == "CLIPS";
                g.setColour(isClips && clippingCount_ > 0 ? KhensuTheme::accentRed : KhensuTheme::textPrimary);
                g.drawText(stats_[i].value, cell.withTrimmedTop(11.0f), juce::Justification::centredLeft, false);
            }
        } else {
            g.setFont(KhensuTheme::labelFont(type::caption));
            g.setColour(KhensuTheme::textTertiary);
            g.drawText(statsFetchInFlight_ ? "Measuring levels..." : "Level stats pending...", ga,
                       juce::Justification::centredLeft, false);
        }
    }

    // Spectral presence bar (Low / Mid-vocal / High)
    if (!specArea_.isEmpty()) {
        auto sa = specArea_.toFloat();
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText("SPECTRAL BALANCE", sa.removeFromTop(12.0f), juce::Justification::centredLeft, false);

        float total = juce::jmax(0.001f, balanceLow_ + balanceMid_ + balanceHigh_);
        float wLow = sa.getWidth() * (balanceLow_ / total);
        float wMid = sa.getWidth() * (balanceMid_ / total);
        float wHigh = sa.getWidth() * (balanceHigh_ / total);
        auto bar = sa.reduced(0.0f, 1.0f).withHeight(8.0f);

        g.setColour(KhensuTheme::accentTeal.withAlpha(0.7f));
        g.fillRoundedRectangle(juce::Rectangle<float>(bar.getX(), bar.getY(), wLow, bar.getHeight()), 3.0f);
        g.setColour(KhensuTheme::accentBlue.withAlpha(0.85f));
        g.fillRoundedRectangle(juce::Rectangle<float>(bar.getX() + wLow, bar.getY(), wMid, bar.getHeight()), 3.0f);
        g.setColour(KhensuTheme::accentAmber.withAlpha(0.7f));
        g.fillRoundedRectangle(juce::Rectangle<float>(bar.getX() + wLow + wMid, bar.getY(), wHigh, bar.getHeight()),
                               3.0f);

        // Vocal range label below bar
        float vocalStartX = bar.getX() + wLow;
        float vocalW = wMid;
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.setColour(KhensuTheme::accentBlue.withAlpha(0.9f));
        g.drawText("VOCAL", juce::Rectangle<float>(vocalStartX, bar.getBottom() + 1.0f, vocalW, 10.0f),
                   juce::Justification::centred, false);
    }
}

void VocalQuickStatsPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    // Reserve the bottom 58px for the spectral bar; the stats grid takes
    // the space between the BPM/Key/Genre row and the bar.
    specArea_ = b.removeFromBottom(58);
    b.removeFromTop(40); // BPM/Key/Genre row
    gridArea_ = b.reduced(0, 2);
}

} // namespace khensu
