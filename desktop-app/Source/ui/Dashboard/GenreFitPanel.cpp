// desktop-app/Source/ui/Dashboard/GenreFitPanel.cpp
#include "GenreFitPanel.h"
#include "../shared/KhensuTheme.h"

namespace khensu {

namespace {
juce::File statsFile()
{
    const auto exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    auto f = exeDir.getChildFile("resources/genre_norms/genre_corpus_stats.json");
    if (!f.existsAsFile())
        f = juce::File::getCurrentWorkingDirectory().getChildFile("resources/genre_norms/genre_corpus_stats.json");
    return f;
}
} // namespace

GenreFitPanel::GenreFitPanel(IAppController& c) : PanelCard(c, TRANS("Genre Fit"))
{
    setExplainer(TRANS("Genre Fit"),
                 TRANS("Where this mix sits among released tracks of its genre. The band is the middle 80% of "
                       "the genre's measured tracks, the tick their median, the dot this mix. The count says "
                       "how many tracks the band rests on - a handful is a hint, not a norm."));
    const auto f = statsFile();
    if (f.existsAsFile()) stats_ = juce::JSON::parse(f);
}

double GenreFitPanel::position(double v, double lo, double hi) noexcept
{
    if (!(hi > lo)) return 0.5;
    return juce::jlimit(0.0, 1.0, (v - lo) / (hi - lo));
}

std::vector<GenreFitPanel::Row> GenreFitPanel::rowsFor(const juce::var& stats, const juce::String& genreId,
                                                       const AnalysisResult& r, int& tracks)
{
    std::vector<Row> out;
    tracks = 0;
    const auto g = stats["genres"][juce::Identifier(genreId.isEmpty() ? juce::String("-") : genreId)];
    if (!g.isObject()) return out;
    tracks = (int)g["n"];
    const bool analysed = r.loudness.lufs_integrated > -90.0f;
    struct Spec {
        const char* key;
        const char* label;
        const char* unit;
        std::optional<double> value;
    };
    const Spec specs[] = {
        {"lufs", NEEDS_TRANS("Loudness"), "LUFS", analysed ? std::optional<double>(r.loudness.lufs_integrated) : std::nullopt},
        {"crest_factor_db", NEEDS_TRANS("Crest"), "dB", analysed ? std::optional<double>(r.dynamics.crest_factor_db) : std::nullopt},
        {"centroid_hz", NEEDS_TRANS("Brightness (centroid)"), "Hz",
         analysed ? std::optional<double>(r.spectral.centroid_hz) : std::nullopt},
        {"tilt_db_per_octave", NEEDS_TRANS("Tilt"), "dB/oct",
         analysed ? std::optional<double>(r.spectral.tilt_db_per_octave) : std::nullopt},
        {"balance_low", NEEDS_TRANS("Low-end share"), "", analysed ? std::optional<double>(r.spectral.balance_low) : std::nullopt},
        {"width", NEEDS_TRANS("Stereo width"), "", analysed ? std::optional<double>(r.stereo.width) : std::nullopt},
    };
    for (const auto& s : specs) {
        const auto m = g[s.key];
        if (!m.isObject()) continue;
        Row row;
        row.label = juce::translate(s.label);
        row.unit = s.unit;
        row.p10 = (double)m["p10"];
        row.median = (double)m["median"];
        row.p90 = (double)m["p90"];
        row.value = s.value;
        out.push_back(row);
    }
    return out;
}

void GenreFitPanel::setData(const AnalysisResult& r)
{
    const auto* session = controller_.getActiveSession();
    genreId_ = session != nullptr && !session->target.primary_genre.empty()
                   ? juce::String(session->target.primary_genre)
                   : juce::String(r.python_data.genre_detected);
    rows_ = rowsFor(stats_, genreId_, r, tracks_);
    repaint();
}

void GenreFitPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (rows_.empty()) {
        drawEmptyState(g, TRANS("No measured norms for this genre"),
                       genreId_.isEmpty() ? TRANS("Choose the session's genre on Intake.")
                                          : TRANS("The corpus has too few tracks of this genre."));
        return;
    }
    auto b = bodyArea();
    g.setFont(KhensuTheme::labelFont(11.0f));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText(genreId_ + " - " + juce::String(tracks_) + " " + TRANS("measured tracks"), b.removeFromTop(16),
               juce::Justification::centredLeft, true);
    const int rowH = juce::jlimit(22, 34, b.getHeight() / juce::jmax(1, (int)rows_.size()));
    for (const auto& row : rows_) {
        auto r = b.removeFromTop(rowH);
        auto label = r.removeFromLeft(140);
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(row.label, label, juce::Justification::centredLeft, true);
        auto valueText = r.removeFromRight(90);
        // The scale holds the band and the mix, with a margin either side.
        double lo = row.p10, hi = row.p90;
        if (row.value) {
            lo = juce::jmin(lo, *row.value);
            hi = juce::jmax(hi, *row.value);
        }
        const double pad = (hi - lo) * 0.15 + 1e-9;
        lo -= pad;
        hi += pad;
        auto track = r.reduced(6, rowH / 2 - 4).toFloat();
        g.setColour(KhensuTheme::bgRaised);
        g.fillRoundedRectangle(track, 3.0f);
        const auto x = [&](double v) { return track.getX() + (float)position(v, lo, hi) * track.getWidth(); };
        // Neutral colours: where a mix sits is information, not a severity.
        g.setColour(KhensuTheme::accentBlue.withAlpha(0.35f));
        g.fillRect(juce::Rectangle<float>(x(row.p10), track.getY(), x(row.p90) - x(row.p10), track.getHeight()));
        g.setColour(KhensuTheme::textSecondary);
        g.drawVerticalLine((int)x(row.median), track.getY() - 2, track.getBottom() + 2);
        if (row.value) {
            g.setColour(KhensuTheme::textPrimary);
            g.fillEllipse(x(*row.value) - 4.0f, track.getCentreY() - 4.0f, 8.0f, 8.0f);
            g.setColour(KhensuTheme::textSecondary);
            const int dp = std::abs(*row.value) >= 100.0 ? 0 : (std::abs(*row.value) >= 1.0 ? 1 : 2);
            g.drawText(juce::String(*row.value, dp) + (row.unit.isEmpty() ? juce::String() : " " + row.unit),
                       valueText, juce::Justification::centredRight, true);
        }
    }
}

} // namespace khensu
