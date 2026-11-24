// desktop-app/Source/ui/Dashboard/TonalBalancePanel.cpp
#include "TonalBalancePanel.h"
#include <khensu/session/Session.h>
#include <khensu/targets/GenreDatabase.h>
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include <cmath>

namespace khensu {

// Center frequencies of the 10 octave bands in rms_per_octave_db.
static constexpr float kBandCentersHz[10] = {31.0f,   63.0f,   125.0f,  250.0f,  500.0f,
                                             1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f};

static juce::String bandLabel(float hz)
{
    return hz >= 1000.0f ? juce::String(hz / 1000.0f, hz >= 10000.0f ? 0 : 0) + "k" : juce::String((int)hz);
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
}

void TonalBalancePanel::setData(const AnalysisResult& r)
{
    measuredDb_.clear();
    targetDb_.clear();
    hasData_ = false;

    const auto& oct = r.spectral.rms_per_octave_db;
    if (oct.size() < 4) {
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
    repaint();
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
        verdictColour_ = KhensuTheme::accentTeal;
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

void TonalBalancePanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    auto b = bodyArea();

    if (!hasData_) {
        drawEmptyState(g, "No tonal balance yet", "Run a full analysis to see the tonal-balance fingerprint.");
        return;
    }

    auto verdictRow = b.removeFromBottom(16);
    auto labelRow = b.removeFromBottom(12);
    auto plot = b.toFloat().reduced(2.0f, 4.0f);

    const int n = (int)measuredDb_.size();
    if (n < 2 || plot.getHeight() < 20.0f) return;

    // Y range: symmetric around 0, covering both curves + tolerance.
    float maxAbs = kToleranceDb + 2.0f;
    for (int i = 0; i < n; ++i) {
        maxAbs = juce::jmax(maxAbs, std::abs(measuredDb_[(size_t)i]));
        maxAbs = juce::jmax(maxAbs, std::abs(targetDb_[(size_t)i]) + kToleranceDb);
    }
    maxAbs += 1.0f;

    auto xAt = [&](int i) { return plot.getX() + plot.getWidth() * (float)i / (float)(n - 1); };
    auto yAt = [&](float v) { return plot.getCentreY() - (v / maxAbs) * plot.getHeight() * 0.5f; };

    // Tolerance band around the target line.
    juce::Path band;
    band.startNewSubPath(xAt(0), yAt(targetDb_[0] + kToleranceDb));
    for (int i = 1; i < n; ++i)
        band.lineTo(xAt(i), yAt(targetDb_[(size_t)i] + kToleranceDb));
    for (int i = n - 1; i >= 0; --i)
        band.lineTo(xAt(i), yAt(targetDb_[(size_t)i] - kToleranceDb));
    band.closeSubPath();
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.08f));
    g.fillPath(band);

    // Target tilt line (dashed).
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.55f));
    for (int i = 1; i < n; ++i) {
        float dashes[] = {4.0f, 3.0f};
        g.drawDashedLine({xAt(i - 1), yAt(targetDb_[(size_t)i - 1]), xAt(i), yAt(targetDb_[(size_t)i])}, dashes, 2,
                         1.2f);
    }

    // Measured curve.
    juce::Path curve;
    curve.startNewSubPath(xAt(0), yAt(measuredDb_[0]));
    for (int i = 1; i < n; ++i)
        curve.lineTo(xAt(i), yAt(measuredDb_[(size_t)i]));
    g.setColour(KhensuTheme::textPrimary.withAlpha(0.9f));
    g.strokePath(curve, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved));

    // Band markers: teal inside tolerance, amber/red outside.
    hover_.clear();
    for (int i = 0; i < n; ++i) {
        const float d = measuredDb_[(size_t)i] - targetDb_[(size_t)i];
        {
            const float hz = 31.25f * std::pow(2.0f, (float)i * 9.0f / (float)juce::jmax(1, n - 1)); // 31 Hz - 16 kHz
            const float w = plot.getWidth() / (float)juce::jmax(1, n - 1);
            hover_.addColumn(xAt(i) - w * 0.5f, xAt(i) + w * 0.5f, plot,
                             (hz >= 1000.0f ? juce::String(hz / 1000.0f, 1) + " kHz" : juce::String(juce::roundToInt(hz)) + " Hz")
                                 + ": " + juce::String(measuredDb_[(size_t)i], 1) + " dB, " + TRANS("target") + " "
                                 + juce::String(targetDb_[(size_t)i], 1) + " dB (" + (d >= 0.0f ? "+" : "")
                                 + juce::String(d, 1) + " dB)");
        }
        const bool out = std::abs(d) > kToleranceDb;
        const bool far_ = std::abs(d) > kToleranceDb + 3.0f;
        g.setColour(!out ? KhensuTheme::accentTeal : far_ ? KhensuTheme::accentRed : KhensuTheme::accentAmber);
        g.fillEllipse(xAt(i) - 2.5f, yAt(measuredDb_[(size_t)i]) - 2.5f, 5.0f, 5.0f);
    }

    // Frequency labels.
    g.setFont(khensu::KhensuTheme::uiFont(8.0f, juce::Font::plain));
    g.setColour(KhensuTheme::textSecondary);
    for (int i = 0; i < n && i < 10; ++i)
        g.drawText(bandLabel(kBandCentersHz[i]), (int)xAt(i) - 14, labelRow.getY(), 28, labelRow.getHeight(),
                   juce::Justification::centred);

    // Verdict footer.
    g.setFont(khensu::KhensuTheme::uiFont(8.5f, juce::Font::bold));
    g.setColour(verdictColour_);
    g.drawText(verdict_, verdictRow, juce::Justification::centredLeft, true);
}

} // namespace khensu
