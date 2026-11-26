// desktop-app/Source/ui/Dashboard/TonalBalancePanel.cpp
#include "TonalBalancePanel.h"
#include <khensu/session/Session.h>
#include <khensu/targets/GenreDatabase.h>
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include <cmath>

namespace khensu {

namespace chart = ui::chart;

// Center frequencies of the 10 octave bands in rms_per_octave_db.
static constexpr float kBandCentersHz[10] = {31.0f,   63.0f,   125.0f,  250.0f,  500.0f,
                                             1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f};

static juce::String bandLabel(float hz)
{
    return hz >= 1000.0f ? juce::String(juce::roundToInt(hz / 1000.0f)) + "k" : juce::String(juce::roundToInt(hz));
}

/// The centre of band i of n (31 Hz - 16 kHz, one octave apart).
static double bandHz(size_t i, size_t n)
{
    return n == 10 ? static_cast<double>(kBandCentersHz[i])
                   : 31.25 * std::pow(2.0, static_cast<double>(i) * 9.0 / static_cast<double>(std::max<size_t>(1, n - 1)));
}

TonalBalancePanel::TonalBalancePanel(IAppController& c) : PanelCard(c, "Tonal Balance Fingerprint")
{
    setExplainer(TRANS("Tonal Balance Fingerprint"),
                 "Your track's long-term octave spectrum (solid curve) compared against the tonal "
                 "tilt this genre typically delivers (dashed line, with a +/-"
                 "3 dB tolerance band). Both curves are level-normalized, so this compares shape, "
                 "not loudness.\n\n"
                 "Bands outside the tolerance band are the ones listeners will notice as 'off' for "
                 "the genre - a bump at 250-"
                 "500 Hz reads as mud, missing "
                 "8-"
                 "16 kHz reads as dull, excess 2-"
                 "4 kHz reads as harsh. "
                 "Use it as a sanity check, not a law: deliberate departures are a mix decision, "
                 "accidental ones are usually a monitoring problem.",
                 "spectral_balance");
    chart_.setTitle(TRANS("Tonal balance against the genre"));
    chart_.x = chart::scales::frequency();
    chart_.setXRange(chart_.x.lo, chart_.x.hi);
    chart_.y = chart::scales::relativeDb();
    chart_.setMargins(44, 18, 10, 20);
    chart_.measured = &measuredDb_;
    chart_.target = &targetDb_;
    addChildComponent(chart_);
}

void TonalBalancePanel::Chart::extendReadout(chart::Readout& r, double hz)
{
    if (measured == nullptr || target == nullptr || series().size() < 2) return;
    const auto m = LineChart::valueAt(series().back(), hz);
    const auto t = LineChart::valueAt(series().front(), hz);
    if (!m || !t) return;
    const double d = *m - *t;
    // VIS-4: the verdict in words, not by colour alone.
    const auto word = std::abs(d) <= kToleranceDb ? TRANS("inside the tolerance")
                      : d > 0.0                    ? TRANS("above the tolerance")
                                                   : TRANS("below the tolerance");
    r.lines.push_back({(d >= 0.0 ? "+" : "") + juce::String(d, 1) + " dB " + TRANS("vs target") + " - " + word,
                       style().text});
}

void TonalBalancePanel::setData(const AnalysisResult& r)
{
    measuredDb_.clear();
    targetDb_.clear();
    hasData_ = false;

    const auto& oct = r.spectral.rms_per_octave_db;
    if (oct.size() < 4) {
        rebuildChart();
        repaint();
        return;
    }

    // Mean-normalize the measured curve so we compare shape, not level.
    float mean = 0.0f;
    for (float v : oct)
        mean += v;
    mean /= (float)oct.size();
    for (float v : oct)
        measuredDb_.push_back(v - mean);

    // Genre target: a straight tilt line in dB vs. log2(f), mean-normalized
    // over the same bands so the two curves share a reference.
    float tiltTarget = -4.5f;
    genreName_ = "generic";
    if (auto* session = controller_.getActiveSession()) {
        if (auto genre = controller_.genreDatabase().findById(session->target.primary_genre)) {
            tiltTarget = genre->spectral_tilt_target_db_per_octave;
            genreName_ = juce::String(genre->name);
        }
    }
    const int n = (int)measuredDb_.size();
    const float midIdx = (n - 1) * 0.5f;
    float tMean = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float v = tiltTarget * ((float)i - midIdx); // one octave per band index
        targetDb_.push_back(v);
        tMean += v;
    }
    tMean /= (float)n;
    for (auto& v : targetDb_)
        v -= tMean;

    hasData_ = true;
    rebuildVerdict();
    rebuildChart();
    repaint();
}

void TonalBalancePanel::rebuildChart()
{
    chart_.setVisible(hasData_);
    if (!hasData_) {
        chart_.clear();
        return;
    }
    const size_t n = measuredDb_.size();
    std::vector<double> hz(n), meas(n), tgt(n), lo(n), hi(n);
    double minV = 0.0, maxV = 0.0;
    std::vector<double> outHz, outDb;
    for (size_t i = 0; i < n; ++i) {
        hz[i] = bandHz(i, n);
        meas[i] = measuredDb_[i];
        tgt[i] = i < targetDb_.size() ? targetDb_[i] : 0.0;
        lo[i] = tgt[i] - kToleranceDb;
        hi[i] = tgt[i] + kToleranceDb;
        minV = std::min({minV, meas[i], lo[i]});
        maxV = std::max({maxV, meas[i], hi[i]});
        if (std::abs(meas[i] - tgt[i]) > kToleranceDb) {
            outHz.push_back(hz[i]);
            outDb.push_back(meas[i]);
        }
    }
    // VIS-3: the standard relative range, narrowed to the data.
    chart_.y = chart::scales::autoFit(chart::scales::relativeDb(), minV, maxV, 3.0, 12.0);

    chart::Band tolerance{TRANS("Tolerance") + " (+/-3 dB)", KhensuTheme::vizSpreadInner, hz, lo, hi};
    chart_.setBands({tolerance});
    chart::Series target{genreName_ + " " + TRANS("target"), KhensuTheme::vizMedian, hz, tgt, chart::Series::Kind::Dashed, 1.3f};
    chart::Series track{TRANS("Track"), KhensuTheme::vizTrack, hz, meas, chart::Series::Kind::Line, 2.2f};
    std::vector<chart::Series> series{target};
    if (!outHz.empty()) { // the bands outside the tolerance, ringed (named in the readout and verdict)
        chart::Series out{TRANS("Outside tolerance"), KhensuTheme::accentAmber, outHz, outDb, chart::Series::Kind::Dots, 2.5f};
        out.inReadout = false;
        series.push_back(out);
    }
    series.push_back(track); // last: on top, and read by extendReadout
    chart_.setSeries(std::move(series));

    // VIS-8: the summary in words.
    juce::StringArray outside;
    for (size_t i = 0; i < n; ++i) {
        const double d = meas[i] - tgt[i];
        if (std::abs(d) > kToleranceDb)
            outside.add(bandLabel(static_cast<float>(hz[i])) + "Hz " + (d > 0 ? "+" : "") + juce::String(d, 1) + " dB");
    }
    juce::String summary = TRANS("Tonal balance against the") + " " + genreName_ + " " + TRANS("target") + ": ";
    summary << (outside.isEmpty() ? TRANS("every octave band inside the +/-3 dB tolerance")
                                  : juce::String(outside.size()) + " " + TRANS("bands outside the tolerance") + ": " +
                                        chart::joinWords(outside))
            << ". " << verdict_;
    chart_.setSummary(summary);
}

void TonalBalancePanel::rebuildVerdict()
{
    float worstDelta = 0.0f;
    int worstBand = -1;
    for (size_t i = 0; i < measuredDb_.size() && i < targetDb_.size(); ++i) {
        const float d = measuredDb_[i] - targetDb_[i];
        if (std::abs(d) > std::abs(worstDelta)) {
            worstDelta = d;
            worstBand = (int)i;
        }
    }
    if (worstBand < 0 || std::abs(worstDelta) <= kToleranceDb) {
        verdict_ = "Balance sits inside the " + genreName_ + " tolerance band.";
        verdictColour_ = KhensuTheme::textSecondary;
        return;
    }
    const float hz = kBandCentersHz[juce::jlimit(0, 9, worstBand)];
    juce::String direction = worstDelta > 0.0f ? (hz <= 500.0f    ? "heavier (possible mud)"
                                                  : hz >= 4000.0f ? "brighter (possible harshness)"
                                                                  : "more forward")
                                               : (hz <= 500.0f    ? "lighter (possible thinness)"
                                                  : hz >= 4000.0f ? "duller (rolled-off top)"
                                                                  : "more scooped");
    verdict_ = juce::String(worstDelta > 0 ? "+" : "") + juce::String(worstDelta, 1) + " dB at " + bandLabel(hz) +
               "Hz - " + direction + " than " + genreName_ + " norm";
    verdictColour_ = std::abs(worstDelta) > kToleranceDb + 3.0f ? KhensuTheme::accentRed : KhensuTheme::accentAmber;
}

void TonalBalancePanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    b.removeFromBottom(18); // the verdict
    chart_.setBounds(b);
}

void TonalBalancePanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!hasData_) {
        drawEmptyState(g, "No tonal balance yet", "Run a full analysis to see the tonal-balance fingerprint.");
        return;
    }
    // Verdict footer: the words carry the meaning; the colour only marks a
    // problem (VIS-4).
    auto verdictRow = bodyArea().removeFromBottom(16);
    g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    g.setColour(verdictColour_);
    g.drawText(verdict_, verdictRow, juce::Justification::centredLeft, true);
}

} // namespace khensu
