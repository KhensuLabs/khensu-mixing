// desktop-app/Source/ui/ExportDelivery/VisualReport.cpp
#include "VisualReport.h"
#include <khensu/ui/VizPalette.h>
#include <cmath>

namespace khensu::visual_report {

namespace {
namespace viz = khensu::ui::viz;

// The light palette, as CSS colours.
juce::String hex(juce::Colour c) { return "#" + c.toDisplayString(false).toLowerCase(); }
const juce::String kInk = "#0b0b0b", kInk2 = "#52514e", kMuted = "#898781", kGrid = "#e1e0d9", kAxis = "#c3c2b7";
const juce::String kSevere = "#d03b3b", kModerate = "#c98500", kMinor = "#2a78d6";

juce::String num(double v, int decimals = 1) { return juce::String(v, decimals); }

juce::String severityColour(float s)
{
    return s >= 66.0f ? kSevere : s >= 33.0f ? kModerate : kMinor;
}

juce::String severityWord(float s)
{
    return s >= 66.0f ? TRANS("Severe") : s >= 33.0f ? TRANS("Moderate") : TRANS("Minor");
}

struct Frame {
    double x0, y0, w, h; ///< plot rectangle inside the SVG
};

double logX(const Frame& f, double hz, double lo = 20.0, double hi = 20000.0)
{
    return f.x0 + f.w * std::log(std::clamp(hz, lo, hi) / lo) / std::log(hi / lo);
}

double linY(const Frame& f, double v, double lo, double hi)
{
    return f.y0 + f.h * (1.0 - (std::clamp(v, lo, hi) - lo) / std::max(1e-9, hi - lo));
}

juce::String svgOpen(int w, int h)
{
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " + juce::String(w) + " " + juce::String(h)
           + "\" width=\"100%\" role=\"img\" style=\"max-width:" + juce::String(w) + "px\">";
}

juce::String text(double x, double y, const juce::String& s, const juce::String& anchor = "start",
                  const juce::String& colour = kMuted, int size = 11)
{
    return "<text x=\"" + num(x) + "\" y=\"" + num(y) + "\" font-size=\"" + juce::String(size) + "\" fill=\"" + colour
           + "\" text-anchor=\"" + anchor + "\">" + escape(s) + "</text>";
}
} // namespace

juce::String escape(const juce::String& s)
{
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;");
}

juce::String tonalBalanceSvg(const hero_spectrum::Model& m, int width, int height)
{
    juce::String s = svgOpen(width, height);
    const Frame f{36.0, 10.0, width - 46.0, height - 34.0};
    float lo = 1e9f, hi = -1e9f;
    for (const auto& p : m.spectrum) {
        lo = std::min(lo, p.db);
        hi = std::max(hi, p.db);
    }
    for (const auto& b : m.spread) {
        lo = std::min(lo, b.p[GenreSpread::kP10]);
        hi = std::max(hi, b.p[GenreSpread::kP90]);
    }
    if (lo > hi) lo = -24.0f, hi = 6.0f;
    const double top = std::ceil((hi + 3.0) / 6.0) * 6.0, bottom = std::floor((lo - 3.0) / 6.0) * 6.0;
    for (const double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
        const double x = logX(f, hz);
        s << "<line x1=\"" << num(x) << "\" y1=\"" << num(f.y0) << "\" x2=\"" << num(x) << "\" y2=\"" << num(f.y0 + f.h)
          << "\" stroke=\"" << kGrid << "\"/>";
        s << text(x, f.y0 + f.h + 14.0, hz >= 1000.0 ? juce::String(juce::roundToInt(hz / 1000.0)) + "k" : juce::String(juce::roundToInt(hz)), "middle");
    }
    for (double db = top; db >= bottom; db -= 12.0)
        s << text(f.x0 - 6.0, linY(f, db, bottom, top) + 4.0, juce::String(juce::roundToInt(db)), "end");
    auto band = [&](int a, int b, const juce::String& fill) {
        juce::String d = "M";
        for (size_t i = 0; i < m.spread.size(); ++i)
            d << (i == 0 ? "" : " L") << num(logX(f, m.spread[i].hz)) << " " << num(linY(f, m.spread[i].p[(size_t)b], bottom, top));
        for (size_t i = m.spread.size(); i-- > 0;)
            d << " L" << num(logX(f, m.spread[i].hz)) << " " << num(linY(f, m.spread[i].p[(size_t)a], bottom, top));
        return "<path d=\"" + d + " Z\" fill=\"" + fill + "\"/>";
    };
    if (m.hasSpread()) {
        s << band(GenreSpread::kP10, GenreSpread::kP90, "rgba(11,11,11,0.07)");
        s << band(GenreSpread::kP25, GenreSpread::kP75, "rgba(11,11,11,0.13)");
    }
    if (m.spectrum.size() >= 2) {
        // The track, coloured by how far it strays from released tracks.
        for (size_t i = 1; i < m.spectrum.size(); ++i) {
            const auto& a = m.spectrum[i - 1];
            const auto& b = m.spectrum[i];
            const float stray = hero_spectrum::strayAt(m, std::sqrt(a.hz * b.hz), 0.5f * (a.db + b.db));
            const auto ink = juce::Colour::fromString("ff0b0b0b"); // the printed page's ink, not the app theme
            const auto colour = std::abs(stray) < 0.05f
                                    ? ink
                                    : ink.interpolatedWith(juce::Colour(stray > 0.0f ? viz::kDivergingWarm : viz::kDivergingCool),
                                                           std::abs(stray));
            s << "<line x1=\"" << num(logX(f, a.hz)) << "\" y1=\"" << num(linY(f, a.db, bottom, top)) << "\" x2=\""
              << num(logX(f, b.hz)) << "\" y2=\"" << num(linY(f, b.db, bottom, top)) << "\" stroke=\"" << hex(colour)
              << "\" stroke-width=\"2\" stroke-linecap=\"round\"/>";
        }
    }
    if (m.reference.size() >= 2) {
        juce::String d = "M";
        for (size_t i = 0; i < m.reference.size(); ++i)
            d << (i == 0 ? "" : " L") << num(logX(f, m.reference[i].hz)) << " " << num(linY(f, m.reference[i].db, bottom, top));
        s << "<path d=\"" << d << "\" fill=\"none\" stroke=\"" << hex(viz::categorical(6, viz::Mode::Light)) << "\" stroke-width=\"1.5\"/>";
    }
    s << "</svg>";
    return s;
}

juce::String dotStripSvg(const hero_spectrum::Gauge& g, int width, int height)
{
    juce::String s = svgOpen(width, height);
    float lo = g.axisLo, hi = g.axisHi;
    if (!g.released.empty()) {
        lo = g.released[(size_t)(0.02 * (double)(g.released.size() - 1))];
        hi = g.released[(size_t)(0.98 * (double)(g.released.size() - 1))];
        if (g.defined) lo = std::min(lo, g.measured), hi = std::max(hi, g.measured);
        const float pad = std::max(1e-3f, (hi - lo) * 0.06f);
        lo -= pad;
        hi += pad;
    }
    auto x = [&](float v) { return 4.0 + (width - 8.0) * std::clamp((v - lo) / std::max(1e-6f, hi - lo), 0.0f, 1.0f); };
    if (g.windowLo && g.windowHi)
        s << "<rect x=\"" << num(x(*g.windowLo)) << "\" y=\"2\" width=\"" << num(std::max(2.0, x(*g.windowHi) - x(*g.windowLo)))
          << "\" height=\"" << juce::String(height - 4) << "\" rx=\"3\" fill=\"rgba(42,120,214,0.12)\"/>";
    juce::Random jitter(17);
    const size_t stride = std::max<size_t>(1, g.released.size() / 200);
    for (size_t k = 0; k < g.released.size(); k += stride)
        s << "<circle cx=\"" << num(x(g.released[k])) << "\" cy=\"" << num(4.0 + jitter.nextFloat() * (height - 8.0)) << "\" r=\"1.4\" fill=\"" << kMuted << "\"/>";
    if (g.defined)
        s << "<rect x=\"" << num(x(g.measured) - 1.5) << "\" y=\"0\" width=\"3\" height=\"" << juce::String(height) << "\" fill=\"" << kInk << "\"/>";
    s << "</svg>";
    return s;
}

juce::String loudnessSvg(const TrackTimeline& tl, const std::vector<SectionData>& sections,
                         std::optional<std::pair<float, float>> window, int width, int height)
{
    juce::String s = svgOpen(width, height);
    const Frame f{36.0, 22.0, width - 46.0, height - 40.0};
    const double dur = std::max(1.0, tl.durationSec), lo = -42.0, hi = 0.0;
    auto tx = [&](double t) { return f.x0 + f.w * t / dur; };
    for (size_t i = 0; i < sections.size(); ++i) {
        const auto& sec = sections[i];
        s << "<rect x=\"" << num(tx(sec.start_sec) + 1) << "\" y=\"2\" width=\"" << num(std::max(1.0, tx(sec.end_sec) - tx(sec.start_sec) - 2))
          << "\" height=\"16\" rx=\"3\" fill=\"" << hex(viz::categorical(static_cast<int>(i) % 8, viz::Mode::Light)) << "\" fill-opacity=\"0.35\"/>";
        if (tx(sec.end_sec) - tx(sec.start_sec) > 40)
            s << text(tx(sec.start_sec) + 4, 14, juce::String(juce::CharPointer_UTF8(sec.label.c_str())), "start", kInk, 10);
    }
    if (window)
        s << "<rect x=\"" << num(f.x0) << "\" y=\"" << num(linY(f, window->second, lo, hi)) << "\" width=\"" << num(f.w)
          << "\" height=\"" << num(linY(f, window->first, lo, hi) - linY(f, window->second, lo, hi)) << "\" fill=\"rgba(6,91,84,0.08)\"/>";
    for (double v = -36.0; v <= -6.0; v += 6.0) {
        s << "<line x1=\"" << num(f.x0) << "\" y1=\"" << num(linY(f, v, lo, hi)) << "\" x2=\"" << num(f.x0 + f.w) << "\" y2=\""
          << num(linY(f, v, lo, hi)) << "\" stroke=\"" << kGrid << "\"/>";
        s << text(f.x0 - 6, linY(f, v, lo, hi) + 4, juce::String(juce::roundToInt(v)), "end");
    }
    juce::String d = "M";
    const size_t step = std::max<size_t>(1, tl.shortTermLufs.size() / 900);
    bool first = true;
    for (size_t i = 0; i < tl.shortTermLufs.size(); i += step) {
        d << (first ? "" : " L") << num(tx(i * tl.hopSec)) << " " << num(linY(f, tl.shortTermLufs[i], lo, hi));
        first = false;
    }
    s << "<path d=\"" << d << "\" fill=\"none\" stroke=\"" << hex(viz::categorical(0, viz::Mode::Light)) << "\" stroke-width=\"1.8\"/>";
    for (const auto& e : tl.truePeakEvents)
        s << "<rect x=\"" << num(tx(e.startSec)) << "\" y=\"" << num(f.y0) << "\" width=\"" << num(std::max(2.0, tx(e.endSec) - tx(e.startSec)))
          << "\" height=\"" << num(f.h) << "\" fill=\"" << kSevere << "\" fill-opacity=\"0.25\"/>";
    for (int m = 0; m <= static_cast<int>(dur / 30.0); ++m) {
        const double t = m * 30.0;
        s << text(tx(t), f.y0 + f.h + 14, juce::String(static_cast<int>(t) / 60) + ":" + juce::String(static_cast<int>(t) % 60).paddedLeft('0', 2), "middle");
    }
    s << "</svg>";
    return s;
}

juce::String differenceSvg(const fix_difference::Result& r, int width, int height)
{
    juce::String s = svgOpen(width, height);
    const Frame f{36.0, 8.0, width - 46.0, height - 26.0};
    double range = 3.0;
    for (const double v : r.spectrumDb)
        range = std::max(range, std::abs(v));
    range = std::ceil(range);
    for (size_t i = 0; i + 1 < r.hz.size(); ++i) {
        const double v = 0.5 * (r.spectrumDb[i] + r.spectrumDb[i + 1]);
        const double x0 = logX(f, r.hz[i], 30.0, 16000.0), x1 = logX(f, r.hz[i + 1], 30.0, 16000.0);
        const double y0 = linY(f, 0.0, -range, range), y1 = linY(f, v, -range, range);
        s << "<rect x=\"" << num(x0) << "\" y=\"" << num(std::min(y0, y1)) << "\" width=\"" << num(std::max(1.0, x1 - x0))
          << "\" height=\"" << num(std::abs(y1 - y0)) << "\" fill=\""
          << hex(viz::diverging(static_cast<float>(v / range), viz::Mode::Light)) << "\"/>";
    }
    s << "<line x1=\"" << num(f.x0) << "\" y1=\"" << num(linY(f, 0.0, -range, range)) << "\" x2=\"" << num(f.x0 + f.w) << "\" y2=\""
      << num(linY(f, 0.0, -range, range)) << "\" stroke=\"" << kAxis << "\"/>";
    s << text(f.x0 - 6, f.y0 + 8, "+" + juce::String(juce::roundToInt(range)), "end");
    s << text(f.x0 - 6, f.y0 + f.h, "-" + juce::String(juce::roundToInt(range)), "end");
    for (const double hz : {100.0, 1000.0, 10000.0})
        s << text(logX(f, hz, 30.0, 16000.0), f.y0 + f.h + 14, hz >= 1000.0 ? juce::String(juce::roundToInt(hz / 1000.0)) + "k" : juce::String(juce::roundToInt(hz)), "middle");
    s << "</svg>";
    return s;
}

juce::String buildHtml(const Input& in)
{
    juce::String h;
    h << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>" << escape(in.trackName)
      << " - " << TRANS("Mix report") << "</title><style>"
      << "body{font-family:system-ui,-apple-system,'Segoe UI',sans-serif;color:" << kInk << ";background:#f9f9f7;margin:0}"
      << "main{max-width:980px;margin:0 auto;padding:32px 24px}h1{font-size:26px;margin:0}h2{font-size:17px;margin:28px 0 8px}"
      << ".muted{color:" << kInk2 << "}.card{background:#fcfcfb;border:1px solid rgba(11,11,11,.1);border-radius:10px;padding:16px;margin:10px 0}"
      << ".score{font-size:44px;font-weight:700}.tiles{display:grid;grid-template-columns:repeat(5,1fr);gap:10px}"
      << ".tile .v{font-size:20px;font-weight:600}.tile .c{font-size:12px;color:" << kInk2 << "}"
      << ".row{display:grid;grid-template-columns:220px 1fr 48px;align-items:center;gap:10px;margin:6px 0}"
      << ".finding{border-left:4px solid;padding:6px 12px;margin:8px 0}.legend span{margin-right:16px;font-size:12px}"
      << "@media print{body{background:#fff}.card{break-inside:avoid}}</style></head><body><main>";
    h << "<h1>" << escape(in.trackName) << "</h1><p class=\"muted\">" << escape(in.genreName.isNotEmpty() ? in.genreName : TRANS("No genre set"))
      << (in.versionName.isNotEmpty() ? " &middot; " + escape(in.versionName) : juce::String()) << " &middot; "
      << escape(in.generatedAt) << "</p>";

    // The verdict.
    h << "<div class=\"card\"><div class=\"muted\">" << TRANS("Readiness") << "</div><div class=\"score\">"
      << juce::roundToInt(in.scorecard.overall_score) << " <span class=\"muted\" style=\"font-size:16px\">/ 100 &middot; "
      << escape(juce::String(in.scorecard.tier_label)) << "</span></div><div class=\"muted\">"
      << escape(juce::String(in.scorecard.tier_audience)) << "</div></div>";

    // Tonal balance.
    h << "<h2>" << TRANS("Tonal balance") << "</h2><div class=\"card\">" << tonalBalanceSvg(in.hero, 920, 300)
      << "<div class=\"legend muted\"><span>&#9644; " << TRANS("this track") << "</span>";
    if (in.hero.hasSpread())
        h << "<span>&#9632; " << TRANS("released") << " " << escape(in.hero.spreadName) << ": " << TRANS("middle 50 % (dark) and 80 % (light)")
          << ", " << juce::String(in.hero.spreadTracks) << " " << TRANS("tracks") << "</span><span style=\"color:" << hex(juce::Colour(viz::kDivergingWarm))
          << "\">" << TRANS("orange: more energy than released tracks") << "</span><span style=\"color:" << hex(juce::Colour(viz::kDivergingCool)) << "\">"
          << TRANS("blue: less") << "</span>";
    h << "</div></div>";

    // Measurements.
    h << "<h2>" << TRANS("Measurements") << "</h2><div class=\"tiles\">";
    for (const auto& g : in.hero.gauges)
        h << "<div class=\"card tile\"><div class=\"muted\">" << escape(g.label) << "</div><div class=\"v\">"
          << escape(g.defined ? g.value : juce::String("-")) << "</div>" << dotStripSvg(g, 160, 18) << "<div class=\"c\">"
          << escape(!g.defined ? TRANS("Not measured on this file") : g.caption.isNotEmpty() ? g.caption : g.windowText) << "</div></div>";
    h << "</div>";

    // Readiness categories.
    h << "<h2>" << TRANS("Readiness by category") << "</h2><div class=\"card\">";
    for (const auto& c : in.scorecard.categories) {
        h << "<div class=\"row\"><div>" << escape(juce::String(c.name)) << "</div><svg viewBox=\"0 0 400 16\" width=\"100%\" height=\"16\">"
          << "<rect x=\"0\" y=\"6\" width=\"400\" height=\"4\" rx=\"2\" fill=\"" << kGrid << "\"/>";
        if (const auto it = in.scorecardSpread.find(c.id); it != in.scorecardSpread.end()) {
            const auto& p = it->second;
            h << "<rect x=\"" << num(4 * p[GenreSpread::kP10]) << "\" y=\"1\" width=\"" << num(4 * (p[GenreSpread::kP90] - p[GenreSpread::kP10]))
              << "\" height=\"14\" rx=\"3\" fill=\"rgba(11,11,11,0.07)\"/><rect x=\"" << num(4 * p[GenreSpread::kP25]) << "\" y=\"1\" width=\""
              << num(4 * (p[GenreSpread::kP75] - p[GenreSpread::kP25])) << "\" height=\"14\" rx=\"3\" fill=\"rgba(11,11,11,0.13)\"/>";
        }
        if (c.available)
            h << "<circle cx=\"" << num(4 * c.score) << "\" cy=\"8\" r=\"5\" fill=\"" << kInk << "\"/>";
        h << "</svg><div>" << (c.available ? juce::String(juce::roundToInt(c.score)) : juce::String("-")) << "</div></div>";
    }
    h << "</div>";

    // Loudness over time.
    if (in.timeline != nullptr && !in.timeline->empty())
        h << "<h2>" << TRANS("Loudness over time") << "</h2><div class=\"card\">"
          << loudnessSvg(*in.timeline, in.sections, in.lufsWindow, 920, 220)
          << "<div class=\"legend muted\"><span>" << TRANS("short-term loudness (LUFS)") << "</span><span>"
          << TRANS("shaded: the genre's loudness window") << "</span><span style=\"color:" << kSevere << "\">"
          << TRANS("red: true peak over -1 dBTP") << "</span></div></div>";

    if (in.difference != nullptr && !in.difference->empty())
        h << "<h2>" << TRANS("What the current version changed") << "</h2><div class=\"card\">" << differenceSvg(*in.difference, 920, 180)
          << "<div class=\"muted\">" << TRANS("Measured against the original: red where energy was added, blue where it was taken away.") << "</div></div>";

    // Findings.
    h << "<h2>" << TRANS("Open findings") << "</h2>";
    if (in.findings.empty()) h << "<p class=\"muted\">" << TRANS("Nothing open.") << "</p>";
    for (const auto& f : in.findings) {
        h << "<div class=\"card finding\" style=\"border-left-color:" << severityColour(f.severity) << "\"><strong>"
          << escape(f.title) << "</strong><div class=\"muted\">" << severityWord(f.severity);
        if (f.moment)
            h << " &middot; " << TRANS("at") << " " << juce::String(static_cast<int>(f.moment->first) / 60) << ":"
              << juce::String(static_cast<int>(f.moment->first) % 60).paddedLeft('0', 2);
        h << "</div>" << (f.action.isNotEmpty() ? "<div>" + escape(f.action) + "</div>" : juce::String()) << "</div>";
    }
    h << "<p class=\"muted\" style=\"margin-top:32px\">" << TRANS("Made with Khensu Mixing. Readiness is distance to the genre's technical targets, not a judgement of the music.")
      << "</p></main></body></html>";
    return h;
}

} // namespace khensu::visual_report
