// desktop-app/Source/ui/ExportDelivery/AlbumConsistencyView.cpp
#include "AlbumConsistencyView.h"
#include "../shared/KhensuEmptyState.h"
#include "../shared/ChartStyle.h"
#include "../shared/FixDifferenceView.h"
#include <algorithm>
#include <cmath>

namespace khensu {

namespace album_consistency {

std::optional<float> integratedLufs(const TrackTimeline& tl)
{
    // BS.1770: momentary blocks (400 ms, 75 % overlap) gated at -70 LUFS,
    // then at -10 LU under the ungated mean.
    std::vector<double> powers;
    const auto step = static_cast<size_t>(std::max(1.0, std::round(0.1 / tl.hopSec)));
    for (size_t i = 3; i < tl.momentaryLufs.size(); i += step)
        if (tl.momentaryLufs[i] > -70.0f) powers.push_back(std::pow(10.0, (tl.momentaryLufs[i] + 0.691) / 10.0));
    if (powers.empty()) return std::nullopt;
    double mean = 0.0;
    for (const double p : powers)
        mean += p;
    mean /= static_cast<double>(powers.size());
    const double gate = mean * std::pow(10.0, -10.0 / 10.0);
    double sum = 0.0;
    int n = 0;
    for (const double p : powers)
        if (p >= gate) {
            sum += p;
            ++n;
        }
    if (n == 0) return std::nullopt;
    return static_cast<float>(-0.691 + 10.0 * std::log10(sum / n));
}

Model build(std::vector<TrackData> tracks, std::vector<float> bandHz)
{
    Model m;
    m.tracks = std::move(tracks);
    m.bandHz = std::move(bandHz);
    std::vector<float> lufs;
    for (const auto& t : m.tracks)
        if (t.lufs) lufs.push_back(*t.lufs);
    if (!lufs.empty()) {
        std::sort(lufs.begin(), lufs.end());
        m.medianLufs = lufs.size() % 2 == 1 ? lufs[lufs.size() / 2]
                                            : 0.5f * (lufs[lufs.size() / 2 - 1] + lufs[lufs.size() / 2]);
    }
    // Each track's shape (normalised to its own mean, so loudness differences
    // stay in the top half) against the album's mean shape.
    const size_t bands = m.bandHz.size();
    std::vector<std::vector<double>> shapes;
    for (const auto& t : m.tracks) {
        if (t.bandDb.size() != bands || bands == 0) {
            shapes.emplace_back();
            continue;
        }
        double mean = 0.0;
        for (const double v : t.bandDb)
            mean += v;
        mean /= static_cast<double>(bands);
        std::vector<double> s(bands);
        for (size_t b = 0; b < bands; ++b)
            s[b] = t.bandDb[b] - mean;
        shapes.push_back(std::move(s));
    }
    std::vector<double> albumMean(bands, 0.0);
    int counted = 0;
    for (const auto& s : shapes)
        if (!s.empty()) {
            for (size_t b = 0; b < bands; ++b)
                albumMean[b] += s[b];
            ++counted;
        }
    for (auto& v : albumMean)
        v = counted > 0 ? v / counted : 0.0;
    for (const auto& s : shapes) {
        std::vector<double> d;
        if (!s.empty())
            for (size_t b = 0; b < bands; ++b) {
                d.push_back(s[b] - albumMean[b]);
                m.maxDeviation = std::max(m.maxDeviation, static_cast<float>(std::abs(d.back())));
            }
        m.deviationDb.push_back(std::move(d));
    }
    return m;
}

} // namespace album_consistency

AlbumConsistencyView::AlbumConsistencyView()
{
    setTitle(TRANS("Album consistency"));
    setTooltip(TRANS("Every track of the project: loudness against the album's median (top) and each track's tonal "
                     "balance against the album's average (bottom; red more, blue less)."));
}

void AlbumConsistencyView::setModel(album_consistency::Model m)
{
    model_ = std::move(m);
    status_.clear();
    repaint();
}

void AlbumConsistencyView::setStatus(const juce::String& s)
{
    status_ = s;
    repaint();
}

juce::Rectangle<float> AlbumConsistencyView::loudArea() const
{
    auto r = getLocalBounds().toFloat().withTrimmedLeft(44.0f).withTrimmedRight(8.0f).withTrimmedTop(18.0f);
    return r.removeFromTop(r.getHeight() * 0.5f).withTrimmedBottom(14.0f);
}

juce::Rectangle<float> AlbumConsistencyView::toneArea() const
{
    auto r = getLocalBounds().toFloat().withTrimmedLeft(44.0f).withTrimmedRight(8.0f).withTrimmedTop(18.0f);
    r.removeFromTop(r.getHeight() * 0.5f);
    return r.withTrimmedBottom(18.0f);
}

float AlbumConsistencyView::xForTrack(int i) const
{
    const auto r = loudArea();
    const int n = std::max(1, static_cast<int>(model_.tracks.size()));
    return r.getX() + r.getWidth() * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
}

void AlbumConsistencyView::paint(juce::Graphics& g)
{
    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    g.drawText(TRANS("Album consistency"), getLocalBounds().removeFromTop(16), juce::Justification::centredLeft, false);
    if (model_.tracks.size() < 2) {
        KhensuEmptyState::paintInto(g, getLocalBounds().withTrimmedTop(18),
                                    status_.isNotEmpty() ? status_
                                                         : TRANS("Add two or more tracks to the project to compare them."),
                                    {}); // UXR-6
        return;
    }
    const auto la = loudArea(), ta = toneArea();
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.4f));
    g.fillRoundedRectangle(la, 3.0f);
    g.fillRoundedRectangle(ta, 3.0f);

    // Loudness: the y range covers every track and the median, 2 LU padded.
    float lo = 0.0f, hi = -60.0f;
    for (const auto& t : model_.tracks) {
        if (t.lufs) lo = std::min(lo, *t.lufs), hi = std::max(hi, *t.lufs);
        if (t.truePeak) hi = std::max(hi, std::min(*t.truePeak, 3.0f));
    }
    if (hi < lo) lo = -20.0f, hi = -6.0f;
    lo -= 2.0f;
    hi += 2.0f;
    auto yL = [&](float v) { return la.getBottom() - (v - lo) / std::max(1.0f, hi - lo) * la.getHeight(); };
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText(juce::String(juce::roundToInt(hi)), juce::Rectangle<float>(0.0f, la.getY() - 6.0f, 40.0f, 12.0f), juce::Justification::centredRight, false);
    g.drawText(juce::String(juce::roundToInt(lo)), juce::Rectangle<float>(0.0f, la.getBottom() - 6.0f, 40.0f, 12.0f), juce::Justification::centredRight, false);
    g.drawText("LUFS", juce::Rectangle<float>(0.0f, la.getCentreY() - 6.0f, 40.0f, 12.0f), juce::Justification::centredRight, false);
    if (model_.medianLufs) {
        const float m = *model_.medianLufs;
        g.setColour(KhensuTheme::vizSpreadInner);
        g.fillRect(juce::Rectangle<float>(la.getX(), yL(m + 1.0f), la.getWidth(), yL(m - 1.0f) - yL(m + 1.0f)));
        g.setColour(KhensuTheme::vizMedian);
        g.drawHorizontalLine(juce::roundToInt(yL(m)), la.getX(), la.getRight());
    }
    juce::Path line;
    bool pen = false;
    for (size_t i = 0; i < model_.tracks.size(); ++i) {
        const auto& t = model_.tracks[i];
        const float x = xForTrack(static_cast<int>(i));
        if (!t.lufs) continue;
        const float y = yL(*t.lufs);
        if (!pen) line.startNewSubPath(x, y);
        else line.lineTo(x, y);
        pen = true;
    }
    g.setColour(KhensuTheme::vizCategory(0).withAlpha(0.5f));
    g.strokePath(line, juce::PathStrokeType(1.2f));
    for (size_t i = 0; i < model_.tracks.size(); ++i) {
        const auto& t = model_.tracks[i];
        const float x = xForTrack(static_cast<int>(i));
        if (t.truePeak) {
            g.setColour(*t.truePeak > -1.0f ? KhensuTheme::accentRed : KhensuTheme::textSecondary);
            g.fillRect(juce::Rectangle<float>(x - 5.0f, yL(std::min(*t.truePeak, hi)) - 1.0f, 10.0f, 2.0f));
        }
        if (t.lufs) {
            const bool out = model_.medianLufs && std::abs(*t.lufs - *model_.medianLufs) > 1.0f;
            g.setColour(out ? KhensuTheme::accentAmber : KhensuTheme::vizCategory(0));
            g.fillEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre({x, yL(*t.lufs)}));
        }
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(juce::String(static_cast<int>(i) + 1), juce::Rectangle<float>(x - 20.0f, ta.getBottom() + 2.0f, 40.0f, 14.0f),
                   juce::Justification::centredTop, false);
    }

    // Tone: per track a column of bands (low at the bottom), diverging.
    const size_t bands = model_.bandHz.size();
    const float colW = ta.getWidth() / static_cast<float>(model_.tracks.size());
    for (size_t i = 0; i < model_.deviationDb.size(); ++i) {
        const auto& d = model_.deviationDb[i];
        if (d.size() != bands || bands == 0) continue;
        const float x0 = ta.getX() + colW * static_cast<float>(i) + 3.0f;
        const float bh = ta.getHeight() / static_cast<float>(bands);
        for (size_t b = 0; b < bands; ++b) {
            const float v = static_cast<float>(d[b]) / model_.maxDeviation;
            g.setColour(KhensuTheme::vizDiverging(v));
            g.fillRect(juce::Rectangle<float>(x0, ta.getBottom() - bh * static_cast<float>(b + 1), colW - 6.0f, bh + 0.5f));
        }
    }
    g.setColour(KhensuTheme::textTertiary);
    g.drawText(TRANS("Tone"), juce::Rectangle<float>(0.0f, ta.getCentreY() - 6.0f, 40.0f, 12.0f), juce::Justification::centredRight, false);
}

juce::String AlbumConsistencyView::readoutFor(int i) const
{
    if (i < 0 || i >= static_cast<int>(model_.tracks.size())) return {};
    const auto& t = model_.tracks[static_cast<size_t>(i)];
    juce::String s = juce::String(i + 1) + ". " + t.name;
    if (t.lufs) {
        s << ": " << juce::String(*t.lufs, 1) << " LUFS";
        if (model_.medianLufs) {
            const float d = *t.lufs - *model_.medianLufs;
            s << " (" << (d >= 0.0f ? "+" : "") << juce::String(d, 1) << " LU " << TRANS("vs the album median") << ")";
        }
    }
    if (t.truePeak) s << ", " << TRANS("true peak") << " " << juce::String(*t.truePeak, 1) << " dBTP";
    if (static_cast<size_t>(i) < model_.deviationDb.size() && !model_.deviationDb[(size_t)i].empty()) {
        const auto& d = model_.deviationDb[(size_t)i];
        const auto hiIt = std::max_element(d.begin(), d.end());
        const auto loIt = std::min_element(d.begin(), d.end());
        ui::chart::Axis hz;
        hz.format = ui::chart::Format::Hz;
        s << ". " << TRANS("Most above the album") << " " << juce::String(*hiIt, 1) << " dB @ "
          << hz.label(model_.bandHz[static_cast<size_t>(hiIt - d.begin())], true) << ", " << TRANS("most below") << " "
          << juce::String(*loIt, 1) << " dB @ " << hz.label(model_.bandHz[static_cast<size_t>(loIt - d.begin())], true);
    }
    return s;
}

void AlbumConsistencyView::mouseMove(const juce::MouseEvent& e)
{
    const auto la = loudArea();
    if (model_.tracks.empty() || e.position.x < la.getX()) {
        setTooltip({});
        return;
    }
    const int i = static_cast<int>((e.position.x - la.getX()) / la.getWidth() * static_cast<float>(model_.tracks.size()));
    setTooltip(readoutFor(juce::jlimit(0, static_cast<int>(model_.tracks.size()) - 1, i)));
}

} // namespace khensu
