// shared-core/src/ui/EqCurveComponent.cpp
// Interactive parametric EQ curve display component.
// Shared between desktop-app and plugin-copilot - see khensu::ui::Palette
// for why this doesn't use desktop-app's full KhensuLookAndFeel.
// THREAD_CONTEXT(UIThread)
#include <khensu/ui/EqCurveComponent.h>
#include <khensu/ui/ChartKit.h>
#include <khensu/ui/KhensuPalette.h>
#include <cmath>
#include <complex>
#include <algorithm>

namespace khensu {

static constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Biquad peaking EQ magnitude response at frequency hz (single band)
// ---------------------------------------------------------------------------
float EqCurveComponent::peakingMagnitudeDb(float fc, float gain_db, float Q, float hz) noexcept
{
    if (hz <= 0.0f || fc <= 0.0f || Q <= 0.0f) return 0.0f;
    // Analog (RBJ prototype) peaking EQ, s normalised to wc:
    //   H(s) = (s^2 + s*A/Q + 1) / (s^2 + s/(A*Q) + 1),  A = 10^(gain/40)
    // |H(jr)|^2 = ((1-r^2)^2 + (A r/Q)^2) / ((1-r^2)^2 + (r/(A Q))^2)
    // At r = 1 this is A^2, i.e. exactly gain_db; it tends to 0 dB far away.
    // (The old expression was not this function: +6 dB drew as ~+2 dB and the
    // radicand went negative below ~0.5*fc, producing NaN.)
    const double r = static_cast<double>(hz) / static_cast<double>(fc);
    const double A = std::pow(10.0, static_cast<double>(gain_db) / 40.0);
    const double q = static_cast<double>(Q);
    const double base = (1.0 - r * r) * (1.0 - r * r);
    const double num = base + (A * r / q) * (A * r / q);
    const double den = base + (r / (A * q)) * (r / (A * q));
    if (den <= 0.0 || num <= 0.0) return 0.0f;
    return static_cast<float>(10.0 * std::log10(num / den));
}

// ---------------------------------------------------------------------------
// RBJ "Audio EQ Cookbook" digital biquad magnitude for any band type
// ---------------------------------------------------------------------------
float EqCurveComponent::bandMagnitudeDb(const EqBand& band, float hz, double sampleRate) noexcept
{
    const double fs = sampleRate > 0.0 ? sampleRate : 44100.0;
    const double nyquist = 0.5 * fs;
    if (hz <= 0.0f || band.hz <= 0.0f || band.q <= 0.0f) return 0.0f;
    // Corner at/above Nyquist: the biquad cannot exist; pedalboard clamps
    // the cutoff below Nyquist, so do the same.
    const double f0 = std::min(static_cast<double>(band.hz), nyquist * 0.999);
    const double f = std::min(static_cast<double>(hz), nyquist * 0.9999);

    const double A = std::pow(10.0, static_cast<double>(band.gain_db) / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * static_cast<double>(band.q));
    const double sqA2a = 2.0 * std::sqrt(A) * alpha;

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;
    switch (band.type) {
        case EqBandType::Peak:
            b0 = 1.0 + alpha * A;
            b1 = -2.0 * cw;
            b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;
            a1 = -2.0 * cw;
            a2 = 1.0 - alpha / A;
            break;
        case EqBandType::LowShelf:
            b0 = A * ((A + 1.0) - (A - 1.0) * cw + sqA2a);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
            b2 = A * ((A + 1.0) - (A - 1.0) * cw - sqA2a);
            a0 = (A + 1.0) + (A - 1.0) * cw + sqA2a;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw);
            a2 = (A + 1.0) + (A - 1.0) * cw - sqA2a;
            break;
        case EqBandType::HighShelf:
            b0 = A * ((A + 1.0) + (A - 1.0) * cw + sqA2a);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
            b2 = A * ((A + 1.0) + (A - 1.0) * cw - sqA2a);
            a0 = (A + 1.0) - (A - 1.0) * cw + sqA2a;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
            a2 = (A + 1.0) - (A - 1.0) * cw - sqA2a;
            break;
        case EqBandType::HighPass:
            b0 = (1.0 + cw) * 0.5;
            b1 = -(1.0 + cw);
            b2 = (1.0 + cw) * 0.5;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cw;
            a2 = 1.0 - alpha;
            break;
        case EqBandType::LowPass:
            b0 = (1.0 - cw) * 0.5;
            b1 = 1.0 - cw;
            b2 = (1.0 - cw) * 0.5;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cw;
            a2 = 1.0 - alpha;
            break;
    }

    // H(e^jw) = (b0 + b1 z^-1 + b2 z^-2) / (a0 + a1 z^-1 + a2 z^-2)
    const double w = 2.0 * kPi * f / fs;
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = z1 * z1;
    const std::complex<double> num = b0 + b1 * z1 + b2 * z2;
    const std::complex<double> den = a0 + a1 * z1 + a2 * z2;
    const double mag = std::abs(den) > 1e-300 ? std::abs(num) / std::abs(den) : 1.0;
    if (!(mag > 0.0)) return -120.0f; // an HPF/LPF null
    return static_cast<float>(std::max(-120.0, 20.0 * std::log10(mag)));
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

EqCurveComponent::EqCurveComponent() { setOpaque(false); }

// ---------------------------------------------------------------------------
// Data setters
// ---------------------------------------------------------------------------

void EqCurveComponent::setBands(std::vector<EqBand> bands)
{
    bands_ = std::move(bands);
    repaint();
}

void EqCurveComponent::setSpectrumUnderlayer(const std::vector<float>& rms)
{
    spectrumDb_ = rms;
    repaint();
}

void EqCurveComponent::setResonanceMarkers(const std::vector<AnalysisResult::ResonancePeak>& peaks)
{
    resonancePeaks_ = peaks;
    repaint();
}

void EqCurveComponent::setGenreReferenceCurve(float centroid_hz, float low_end_weight)
{
    genreCentroid_ = centroid_hz;
    genreLowEnd_ = low_end_weight;
    repaint();
}

void EqCurveComponent::setRecommendationAnnotations(std::vector<EqAnnotation> annotations)
{
    // Most-severe last so its chip and region wash paint on top of milder ones.
    std::sort(annotations.begin(), annotations.end(),
              [](const EqAnnotation& a, const EqAnnotation& b) { return a.severity < b.severity; });
    annotations_ = std::move(annotations);
    hoveredAnnotation_ = -1;
    repaint();
}

// Severity → colour, matching the Mix Notes system (red severe / amber
// moderate / blue minor).
static juce::Colour severityColour(float severity) noexcept
{
    if (severity >= 66.0f) return khensu::ui::Palette::statusBad;
    if (severity >= 33.0f) return khensu::ui::Palette::statusWarn;
    return khensu::ui::Palette::accentBlue;
}

// ---------------------------------------------------------------------------
// Coordinate helpers
// ---------------------------------------------------------------------------

juce::Rectangle<float> EqCurveComponent::plotArea() const noexcept
{
    return getLocalBounds().reduced(8, 4).toFloat().withTrimmedBottom(16.0f); // frequency axis labels at the bottom
}

float EqCurveComponent::xForHz(float hz) const noexcept
{
    auto p = plotArea();
    const float logMin = std::log10(kMinHz);
    const float logMax = std::log10(kMaxHz);
    return p.getX() + (std::log10(juce::jmax(kMinHz, hz)) - logMin) / (logMax - logMin) * p.getWidth();
}

float EqCurveComponent::yForDb(float db) const noexcept
{
    auto p = plotArea();
    const float t = (db - kMaxDb) / (kMinDb - kMaxDb);
    return p.getY() + t * p.getHeight();
}

float EqCurveComponent::hzForX(float x) const noexcept
{
    auto p = plotArea();
    const float logMin = std::log10(kMinHz);
    const float logMax = std::log10(kMaxHz);
    // Guard a zero-width plot (drag before the first layout) - dividing by 0
    // used to write NaN into band.hz.
    const float frac = p.getWidth() > 0.0f ? juce::jlimit(0.0f, 1.0f, (x - p.getX()) / p.getWidth()) : 0.5f;
    return std::pow(10.0f, logMin + frac * (logMax - logMin));
}

float EqCurveComponent::dbForY(float y) const noexcept
{
    auto p = plotArea();
    const float frac = juce::jlimit(0.0f, 1.0f, (y - p.getY()) / p.getHeight());
    return kMaxDb + frac * (kMinDb - kMaxDb);
}

float EqCurveComponent::computeMagnitudeDb(float hz) const noexcept
{
    float total = 0.0f;
    for (const auto& b : bands_)
        if (b.enabled) total += EqCurveComponent::bandMagnitudeDb(b, hz, spectrumSampleRate_);
    return total;
}

int EqCurveComponent::hitTestBand(juce::Point<float> pos) const noexcept
{
    for (int i = (int)bands_.size() - 1; i >= 0; --i) {
        const float bx = xForHz(bands_[i].hz);
        const float by = yForDb(bands_[i].gain_db);
        if (pos.getDistanceFrom({bx, by}) <= kHandleRadius + 2.0f) return i;
    }
    return -1;
}

juce::Rectangle<float> EqCurveComponent::annotationChipBounds(const EqAnnotation& a) const noexcept
{
    auto p = plotArea();
    const float cx = juce::jlimit(p.getX() + 8.0f, p.getRight() - 8.0f, (xForHz(a.low_hz) + xForHz(a.high_hz)) * 0.5f);
    constexpr float kChip = 15.0f;
    return {cx - kChip * 0.5f, p.getY() + 3.0f, kChip, kChip};
}

int EqCurveComponent::hitTestAnnotation(juce::Point<float> pos) const noexcept
{
    // Front-most (last drawn = most severe) wins.
    for (int i = (int)annotations_.size() - 1; i >= 0; --i)
        if (annotationChipBounds(annotations_[i]).expanded(2.0f).contains(pos)) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Curve paths
// ---------------------------------------------------------------------------

juce::Path EqCurveComponent::buildCurvePath() const
{
    auto p = plotArea();
    const int steps = std::max(4, (int)p.getWidth());
    juce::Path path;
    for (int i = 0; i <= steps; ++i) {
        const float x = p.getX() + (float)i / steps * p.getWidth();
        const float hz = hzForX(x);
        const float db = juce::jlimit(kMinDb, kMaxDb, computeMagnitudeDb(hz));
        const float y = yForDb(db);
        if (i == 0)
            path.startNewSubPath(x, y);
        else
            path.lineTo(x, y);
    }
    return path;
}

juce::Path EqCurveComponent::buildSpectrumPath() const
{
    if (spectrumDb_.empty()) return {};
    auto p = plotArea();
    juce::Path path;

    if (spectrumDb_.size() > 100) {
        const int n = (int)spectrumDb_.size();
        const float binHz = static_cast<float>(spectrumSampleRate_) / (float)(n * 2);
        bool first = true;

        for (int i = 0; i < n; ++i) {
            float hz = static_cast<float>(i) * binHz;
            if (hz < kMinHz) continue;
            if (hz > kMaxHz) break;

            float x = xForHz(hz);
            float db = juce::jlimit(kMinDb, kMaxDb, spectrumDb_[(size_t)i] + 18.0f);
            float y = yForDb(db);

            if (first) {
                path.startNewSubPath(x, y);
                first = false;
            } else {
                path.lineTo(x, y);
            }
        }
        if (!path.isEmpty()) {
            path.lineTo(path.getCurrentPosition().x, p.getBottom());
            path.lineTo(xForHz(kMinHz), p.getBottom());
            path.closeSubPath();
        }
    } else {
        static const float kCentreHz[10] = {31.25f,  62.5f,   125.0f,  250.0f,  500.0f,
                                            1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f};
        path.startNewSubPath(p.getX(), p.getBottom());
        const int n = (int)std::min(spectrumDb_.size(), (size_t)10);
        for (int i = 0; i < n; ++i) {
            const float x = xForHz(kCentreHz[i]);
            const float db = juce::jlimit(kMinDb, kMaxDb, spectrumDb_[(size_t)i] + 18.0f);
            path.lineTo(x, yForDb(db));
        }
        path.lineTo(p.getRight(), p.getBottom());
        path.closeSubPath();
    }
    return path;
}

juce::Path EqCurveComponent::buildReferencePath() const
{
    if (genreCentroid_ <= 0.0f) return {};
    auto p = plotArea();
    const int steps = std::max(4, (int)p.getWidth());
    juce::Path path;
    for (int i = 0; i <= steps; ++i) {
        const float x = p.getX() + (float)i / steps * p.getWidth();
        const float hz = hzForX(x);
        // Simple 1/f slope + genre-specific low-end shelf approximation
        float db = -3.0f * std::log2(hz / genreCentroid_);
        if (hz < 200.0f) db += (genreLowEnd_ - 0.5f) * 6.0f * (1.0f - hz / 200.0f);
        db = juce::jlimit(kMinDb, kMaxDb, db);
        const float y = yForDb(db);
        if (i == 0)
            path.startNewSubPath(x, y);
        else
            path.lineTo(x, y);
    }
    return path;
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void EqCurveComponent::paint(juce::Graphics& g)
{
    auto p = plotArea();

    // --- Background gradient (deep slate dark blue) ---
    // Palette tokens only (CORE-20) - these two were hard-coded hex values the
    // theme drift gate never saw.
    juce::ColourGradient bgGrad(khensu::ui::Palette::bgVoid, 0.0f, 0.0f, khensu::ui::Palette::bgBase, 0.0f,
                                p.getHeight(), false);
    g.setGradientFill(bgGrad);
    g.fillRect(p);

    // Specular border outline
    g.setColour(khensu::ui::Palette::borderActive);
    g.drawRect(p, 1.0f);

    // Horizontal 0 dB line (solid, visible)
    g.setColour(khensu::ui::Palette::textMuted.withAlpha(0.20f));
    g.drawHorizontalLine((int)yForDb(0.0f), p.getX(), p.getRight());

    // Frequency grid lines (dotted, modern)
    g.setColour(khensu::ui::Palette::textMuted.withAlpha(0.12f));
    // createDashedStroke() returns a FILLED OUTLINE of the dashes (it ends in
    // createStrokedPath), so the result must be fillPath()'d. Stroking it again
    // outlined every dash instead of drawing it, turning the decade gridlines
    // into hollow 3px pills. Source and destination were also the same Path,
    // which reads as an in-place transform but is not what the API does.
    float dashes[] = {4.0f, 4.0f};
    for (float hz : {100.0f, 1000.0f, 10000.0f}) {
        const float x = xForHz(hz);
        juce::Path gridLine;
        gridLine.startNewSubPath(x, p.getY());
        gridLine.lineTo(x, p.getBottom());
        juce::Path dashedGrid;
        juce::PathStrokeType(1.0f).createDashedStroke(dashedGrid, gridLine, dashes, 2);
        g.fillPath(dashedGrid);
    }

    // --- Recommendation annotation regions (severity-washed vertical bands) ---
    for (const auto& a : annotations_) {
        // Order the pair before mapping: an annotation whose high_hz < low_hz
        // used to collapse to a 2px sliver pinned at the low edge instead of
        // spanning the range. Both edges are then clamped INTO the plot, so a
        // band that starts past the right edge can't paint outside it, and the
        // 2px minimum can't push the wash beyond the plot either
        // (kimi audit P0 #8).
        const float loHz = juce::jmin(a.low_hz, a.high_hz);
        const float hiHz = juce::jmax(a.low_hz, a.high_hz);
        const float x0 = juce::jlimit(p.getX(), p.getRight(), xForHz(loHz));
        const float x1 = juce::jlimit(p.getX(), p.getRight(), xForHz(hiHz));
        const float w = juce::jmin(juce::jmax(2.0f, x1 - x0), p.getRight() - x0);
        const juce::Colour c = severityColour(a.severity);
        g.setColour(c.withAlpha(0.06f));
        g.fillRect(x0, p.getY(), w, p.getHeight());
        // Bounding edges so a wide "broadband" wash still reads as a range.
        // roundToInt, not truncation - (int) biases every edge half a pixel
        // left and blurs the 1px line (kimi audit P1 #30).
        g.setColour(c.withAlpha(0.30f));
        g.drawVerticalLine(juce::roundToInt(x0), p.getY(), p.getBottom());
        g.drawVerticalLine(juce::roundToInt(x1), p.getY(), p.getBottom());
    }

    // --- Spectrum underlay (glowing gradient and line) ---
    if (!spectrumDb_.empty()) {
        juce::Path specPath = buildSpectrumPath();

        // Fill under the spectrum with a subtle translucent dark-teal gradient
        juce::ColourGradient specGrad(khensu::ui::Palette::accentTeal.withAlpha(0.15f), p.getX(), p.getY(),
                                      khensu::ui::Palette::accentTeal.withAlpha(0.01f), p.getX(), p.getBottom(), false);
        g.setGradientFill(specGrad);
        g.fillPath(specPath);

        // Neon outline for the spectrum curve
        g.setColour(khensu::ui::Palette::accentTeal.withAlpha(0.55f));
        g.strokePath(specPath, juce::PathStrokeType(1.2f));
    }

    // --- Genre reference curve (dashed bright teal) ---
    if (genreCentroid_ > 0.0f) {
        juce::Path refPath = buildReferencePath();
        float dashLengths[] = {6.0f, 5.0f};
        juce::Path dashed;
        juce::PathStrokeType(1.5f).createDashedStroke(dashed, refPath, dashLengths, 2);
        g.setColour(khensu::ui::Palette::accentTeal.withAlpha(0.55f));
        g.fillPath(dashed); // already an outline - see the gridline note above
    }

    // --- EQ curve (thick, glowing) ---
    if (!bands_.empty()) {
        juce::Path curvePath = buildCurvePath();

        // Shaded fill below/above 0 dB
        juce::Path fillPath = curvePath;
        fillPath.lineTo(p.getRight(), yForDb(0.0f));
        fillPath.lineTo(p.getX(), yForDb(0.0f));
        fillPath.closeSubPath();

        juce::ColourGradient grad(khensu::ui::Palette::accentTeal.withAlpha(0.24f), p.getX(), p.getY(),
                                  khensu::ui::Palette::accentTeal.withAlpha(0.02f), p.getX(), p.getBottom(), false);
        g.setGradientFill(grad);
        g.fillPath(fillPath);

        // Neon glowing outline curve (2.5f width)
        g.setColour(khensu::ui::Palette::accentTeal);
        g.strokePath(curvePath, juce::PathStrokeType(2.5f));
    }

    // --- Resonance hazard markers (amber downward arrows) ---
    for (const auto& peak : resonancePeaks_) {
        const float x = xForHz(peak.hz);
        const float y = p.getY() + 6.0f;
        juce::Path tri;
        tri.addTriangle(x - 5.0f, y, x + 5.0f, y, x, y + 7.0f);
        g.setColour(khensu::ui::Palette::statusWarn);
        g.fillPath(tri);
        if (peak.prominence > 3.0f) {
            g.setFont(khensu::ui::Palette::uiFont(khensu::type::caption));
            g.drawText(juce::String((int)peak.hz) + "Hz", (int)(x - 20), (int)(y + 8), 40, 10,
                       juce::Justification::centred, false);
        }
    }

    // --- Band handles ---
    for (int i = 0; i < (int)bands_.size(); ++i) {
        const auto& b = bands_[i];
        if (!b.enabled) continue;
        const float bx = xForHz(b.hz);
        const float by = yForDb(b.gain_db);

        const bool hovered = (i == hoveredBand_);
        const bool dragging = (i == dragBandIndex_);
        const bool suggested = b.is_suggested;

        juce::Colour handleCol =
            suggested ? khensu::ui::Palette::textMuted.withAlpha(0.75f) : khensu::ui::Palette::accentTeal;
        if (hovered || dragging) handleCol = handleCol.brighter(0.35f);

        g.setColour(khensu::ui::Palette::bgPanel.darker(0.2f));
        g.fillEllipse(bx - kHandleRadius, by - kHandleRadius, kHandleRadius * 2.0f, kHandleRadius * 2.0f);
        g.setColour(handleCol);
        g.drawEllipse(bx - kHandleRadius, by - kHandleRadius, kHandleRadius * 2.0f, kHandleRadius * 2.0f, 2.0f);

        if (hovered || dragging) {
            g.setFont(khensu::ui::Palette::uiFont(khensu::type::caption));
            g.setColour(khensu::ui::Palette::textPrimary.withAlpha(0.9f));
            juce::String tip =
                juce::String((int)b.hz) + "Hz  " + (b.gain_db >= 0.0f ? "+" : "") + juce::String(b.gain_db, 1) + "dB";
            g.drawText(tip, (int)(bx - 30), (int)(by - kHandleRadius - 14), 60, 12, juce::Justification::centred,
                       false);
        }
    }

    // --- Frequency axis labels ---
    g.setFont(khensu::ui::Palette::uiFont(khensu::type::caption));
    g.setColour(khensu::ui::Palette::textMuted.withAlpha(0.7f));
    for (auto [hz, label] : std::initializer_list<std::pair<float, const char*>>{{50.0f, "50"},
                                                                                 {100.0f, "100"},
                                                                                 {200.0f, "200"},
                                                                                 {500.0f, "500"},
                                                                                 {1000.0f, "1k"},
                                                                                 {2000.0f, "2k"},
                                                                                 {5000.0f, "5k"},
                                                                                 {10000.0f, "10k"},
                                                                                 {20000.0f, "20k"}}) {
        const float x = xForHz(hz);
        g.drawText(label, (int)(x - 12), (int)(p.getBottom() + 3), 24, 12, juce::Justification::centred, false);
    }

    // dB labels on the right
    for (float db : {-12.0f, -6.0f, 0.0f, 6.0f, 12.0f}) {
        const float y = yForDb(db);
        g.drawText((db >= 0 ? "+" : "") + juce::String((int)db), (int)(p.getRight() + 2), (int)(y - 6), 20, 12,
                   juce::Justification::centredLeft, false);
    }

    // --- Annotation warning chips (drawn last so they sit above the curve) ---
    for (int i = 0; i < (int)annotations_.size(); ++i) {
        const auto& a = annotations_[i];
        const auto ch = annotationChipBounds(a);
        const juce::Colour c = severityColour(a.severity);
        const bool hovered = (i == hoveredAnnotation_);

        g.setColour(khensu::ui::Palette::bgPanel.withAlpha(0.9f));
        g.fillEllipse(ch);
        g.setColour(hovered ? c.brighter(0.4f) : c);
        g.drawEllipse(ch, hovered ? 2.0f : 1.5f);
        g.setFont(khensu::ui::Palette::uiFont(khensu::type::caption, juce::Font::bold));
        g.drawText("!", ch, juce::Justification::centred, false);
    }

    // VIZ-U3: the crosshair readout - the frequency and what the EQ does there.
    if (hoverPoint_ && hoveredBand_ < 0 && hoveredAnnotation_ < 0 && p.contains(*hoverPoint_)) {
        const float hz = hzForX(hoverPoint_->x);
        const float db = computeMagnitudeDb(hz);
        const float x = hoverPoint_->x, y = yForDb(db);
        g.setColour(khensu::ui::Palette::textPrimary.withAlpha(0.35f));
        g.drawVerticalLine(juce::roundToInt(x), p.getY(), p.getBottom());
        g.setColour(khensu::ui::Palette::accentTeal);
        g.fillEllipse(x - 3.5f, y - 3.5f, 7.0f, 7.0f);
        ui::chart::Axis fx;
        fx.format = ui::chart::Format::Hz;
        ui::chart::Readout r;
        r.title = fx.label(hz, true);
        r.lines.push_back({TRANS("EQ") + " " + (db >= 0.0f ? "+" : "") + juce::String(db, 1) + " dB",
                           khensu::ui::Palette::accentTeal});
        int nearest = -1;
        float nearestOct = 1e9f;
        for (size_t i = 0; i < bands_.size(); ++i) {
            const float oct = std::abs(std::log2(std::max(1.0f, bands_[i].hz) / hz));
            if (bands_[i].enabled && oct < nearestOct) nearestOct = oct, nearest = static_cast<int>(i);
        }
        if (nearest >= 0 && nearestOct < 1.0f) {
            const auto& b = bands_[(size_t)nearest];
            r.lines.push_back({TRANS("Band") + " " + juce::String(nearest + 1) + ": " + fx.label(b.hz, true) + ", "
                                   + (b.gain_db >= 0.0f ? "+" : "") + juce::String(b.gain_db, 1) + " dB, Q "
                                   + juce::String(b.q, 2),
                               khensu::ui::Palette::textMuted});
        }
        ui::chart::ChartView::paintReadout(g, r, {x, y}, getLocalBounds().toFloat(), ui::chart::Style::fromPalette());
    }

    // Hover tooltip for the focused annotation - the plain-English "what".
    if (hoveredAnnotation_ >= 0 && hoveredAnnotation_ < (int)annotations_.size()) {
        const auto& a = annotations_[hoveredAnnotation_];
        const auto ch = annotationChipBounds(a);
        const juce::Colour c = severityColour(a.severity);

        g.setFont(khensu::ui::Palette::uiFont(khensu::type::caption));
        const int textW =
            juce::jmin((int)p.getWidth() - 12,
                       juce::jmax(120, juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), a.label) + 16));
        const int textH = 30;
        int tx = (int)juce::jlimit(p.getX() + 2.0f, p.getRight() - textW - 2.0f, ch.getCentreX() - textW * 0.5f);
        int ty = (int)ch.getBottom() + 4;
        juce::Rectangle<int> box(tx, ty, textW, textH);

        g.setColour(khensu::ui::Palette::bgPanel.withAlpha(0.96f));
        g.fillRoundedRectangle(box.toFloat(), 4.0f);
        g.setColour(c.withAlpha(0.6f));
        g.drawRoundedRectangle(box.toFloat(), 4.0f, 1.0f);
        g.setColour(khensu::ui::Palette::textPrimary.withAlpha(0.92f));
        g.drawFittedText(a.label, box.reduced(6, 3), juce::Justification::centredLeft, 2);
    }
}

// ---------------------------------------------------------------------------
// Mouse interaction
// ---------------------------------------------------------------------------

void EqCurveComponent::mouseDown(const juce::MouseEvent& e)
{
    dragBandIndex_ = hitTestBand(e.position);
    if (dragBandIndex_ >= 0) {
        dragStartPos_ = e.position;
        dragStartGain_ = bands_[dragBandIndex_].gain_db;
        dragStartHz_ = bands_[dragBandIndex_].hz;
        if (onBandDragStarted_) onBandDragStarted_(dragBandIndex_, bands_[(size_t)dragBandIndex_]);
    }
}

void EqCurveComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (dragBandIndex_ < 0 || dragBandIndex_ >= (int)bands_.size()) return;
    auto& band = bands_[dragBandIndex_];

    // Horizontal drag → frequency (log scale)
    const float newHz = juce::jlimit(kMinHz, kMaxHz, hzForX(e.position.x));
    band.hz = newHz;

    // Vertical drag → gain. Guard the plot height: a zero-height plot (a panel
    // laid out before its first real resize) made this divide by zero and drove
    // the band gain to NaN, which then poisoned the whole curve (P0 #9).
    const float plotH = plotArea().getHeight();
    if (plotH > 0.0f) {
        const float dy = e.position.y - dragStartPos_.y;
        const float dbRange = kMaxDb - kMinDb;
        const float gain = dragStartGain_ - dy / plotH * dbRange;
        band.gain_db = juce::jlimit(kMinDb, kMaxDb, gain);
    }

    repaint();
    if (onBandChanged_) onBandChanged_(dragBandIndex_, band);
}

void EqCurveComponent::mouseUp(const juce::MouseEvent&)
{
    const int index = dragBandIndex_;
    dragBandIndex_ = -1;
    if (index >= 0 && index < (int)bands_.size() && onBandDragEnded_) onBandDragEnded_(index, bands_[(size_t)index]);
}

void EqCurveComponent::mouseMove(const juce::MouseEvent& e)
{
    const int h = hitTestBand(e.position);
    // Band handles win over annotation chips when they overlap.
    const int a = (h >= 0) ? -1 : hitTestAnnotation(e.position);
    hoverPoint_ = e.position;
    if (h != hoveredBand_ || a != hoveredAnnotation_ || (h < 0 && a < 0)) {
        hoveredBand_ = h;
        hoveredAnnotation_ = a;
        repaint();
    }
    setMouseCursor(h >= 0   ? juce::MouseCursor::UpDownLeftRightResizeCursor
                   : a >= 0 ? juce::MouseCursor::PointingHandCursor
                            : juce::MouseCursor::NormalCursor);
}

void EqCurveComponent::mouseExit(const juce::MouseEvent&)
{
    if (hoverPoint_) {
        hoverPoint_.reset();
        repaint();
    }
    if (hoveredBand_ != -1 || hoveredAnnotation_ != -1) {
        hoveredBand_ = -1;
        hoveredAnnotation_ = -1;
        repaint();
    }
}

} // namespace khensu
