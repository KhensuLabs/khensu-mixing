// desktop-app/Source/ui/shared/LiveSpectrogramView.cpp
#include "LiveSpectrogramView.h"
#include "../../IAppController.h"
#include "../../session/PlaybackMeterTap.h"
#include <khensu/ui/VizPalette.h>
#include "VizMaps.h"
#include <algorithm>
#include <cmath>

namespace khensu {

namespace {
constexpr int kHistoryColumns = 720;
constexpr float kRangeDb = 60.0f;

juce::Colour severityColour(float s)
{
    if (s >= 66.0f) return KhensuTheme::accentRed;
    if (s >= 33.0f) return KhensuTheme::accentAmber;
    return KhensuTheme::accentBlue;
}
} // namespace

LiveSpectrogramView::LiveSpectrogramView(IAppController& controller) : controller_(controller)
{
    setTitle(TRANS("Live spectrogram"));
    setDescription(TRANS("A scrolling spectrogram of what is playing, with every frequency finding drawn as a band"));
    setTooltip(TRANS("What is playing, newest on the right. The bands are the open findings' frequency ranges."));
    history_ = juce::Image(juce::Image::RGB, kHistoryColumns, kBands, true);
    window_.resize(kFrame);
    for (int i = 0; i < kFrame; ++i)
        window_[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * static_cast<float>(i) / (kFrame - 1));
    fftBuf_.assign(2 * kFrame, 0.0f);
    juce::Image::BitmapData px(history_, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < kBands; ++y)
        for (int x = 0; x < kHistoryColumns; ++x)
            px.setPixelColour(x, y, ui::viz::spectrogram(0.0f));
}

LiveSpectrogramView::~LiveSpectrogramView()
{
    stopTimer();
    gl_.detach();
}

void LiveSpectrogramView::visibilityChanged()
{
    if (isShowing()) {
        // GPU composition: the component's drawing goes through OpenGL.
        if (!gl_.isAttached()) {
            gl_.setComponentPaintingEnabled(true);
            gl_.setContinuousRepainting(false);
            gl_.attachTo(*this);
        }
        refreshOverlays();
        wasPlaying_ = true; // the first tick settles the rate
        startTimerHz(30);
    } else {
        stopTimer();
        if (gl_.isAttached()) gl_.detach();
    }
}

void LiveSpectrogramView::resized() {}

juce::Rectangle<float> LiveSpectrogramView::plotArea() const noexcept
{
    auto r = getLocalBounds().toFloat();
    r.removeFromLeft(40.0f);
    r.removeFromTop(22.0f);
    r.removeFromBottom(6.0f);
    r.removeFromRight(ui::viz::kColourBarW); // the colour bar
    return r;
}

float LiveSpectrogramView::yForHz(float hz) const noexcept
{
    const auto r = plotArea();
    const float t = std::log(juce::jlimit(kLoHz, kHiHz, hz) / kLoHz) / std::log(kHiHz / kLoHz);
    return r.getBottom() - t * r.getHeight();
}

void LiveSpectrogramView::setOverlays(std::vector<Band> bands, std::vector<Moment> moments)
{
    bands_ = std::move(bands);
    moments_ = std::move(moments);
    repaint();
}

void LiveSpectrogramView::refreshOverlays()
{
    std::vector<Band> bands;
    std::vector<Moment> moments;
    for (const auto& i : controller_.issues().list()) {
        if (i.settled() || i.rec.type == RecommendationType::Strength) continue;
        const juce::String label = juce::String(juce::CharPointer_UTF8(i.rec.what.c_str())).upToFirstOccurrenceOf(".", false, false);
        if (i.rec.freq_range && i.rec.freq_range->is_valid() && i.rec.freq_range->high_hz > 0.0f)
            bands.push_back({i.rec.freq_range->low_hz, i.rec.freq_range->high_hz, i.rec.severity, label});
        if (i.rec.time_range && i.rec.time_range->end_sec > i.rec.time_range->start_sec)
            moments.push_back({i.rec.time_range->start_sec, i.rec.time_range->end_sec, i.rec.severity, label});
    }
    setOverlays(std::move(bands), std::move(moments));
}

void LiveSpectrogramView::pushSamples(const float* left, const float* right, int n, double sampleRate)
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    if (sr != sampleRate_ || binLo_[0] == 0) {
        // The log bands' FFT bins, worked out once per sample rate.
        sampleRate_ = sr;
        for (int b = 0; b < kBands; ++b) {
            const double f0 = kLoHz * std::pow(kHiHz / kLoHz, static_cast<double>(b) / kBands);
            const double f1 = kLoHz * std::pow(kHiHz / kLoHz, static_cast<double>(b + 1) / kBands);
            const int k0 = std::max(1, static_cast<int>(f0 * kFrame / sampleRate_));
            binLo_[static_cast<size_t>(b)] = k0;
            binHi_[static_cast<size_t>(b)] = std::min(kFrame / 2, std::max(k0, static_cast<int>(f1 * kFrame / sampleRate_)));
        }
    }
    // STU-1: a fixed ring of one frame; a full frame is read out and one hop
    // dropped, so nothing is allocated or shifted on this path.
    for (int i = 0; i < n; ++i) {
        if (!pending_.push(0.5f * (left[i] + right[i]))) continue;
        pending_.readFrame(fftBuf_.data());
        std::fill(fftBuf_.begin() + kFrame, fftBuf_.end(), 0.0f);
        for (int j = 0; j < kFrame; ++j)
            fftBuf_[static_cast<size_t>(j)] *= window_[static_cast<size_t>(j)];
        fft_.performFrequencyOnlyForwardTransform(fftBuf_.data(), true);
        const double norm = 4.0 / (static_cast<double>(kFrame) * kFrame);
        for (int b = 0; b < kBands; ++b) {
            const int k0 = binLo_[static_cast<size_t>(b)], k1 = binHi_[static_cast<size_t>(b)];
            double sum = 0.0;
            for (int k = k0; k <= k1; ++k)
                sum += static_cast<double>(fftBuf_[static_cast<size_t>(k)]) * fftBuf_[static_cast<size_t>(k)];
            bandDb_[static_cast<size_t>(b)] = static_cast<float>(10.0 * std::log10(sum * norm / (k1 - k0 + 1) + 1e-14));
        }
        writeColumn(bandDb_);
        pending_.advance();
    }
}

void LiveSpectrogramView::writeColumn(const std::array<float, kBands>& bandDb)
{
    // The colour scale follows the music's own top (slow), 60 dB deep.
    const float top = *std::max_element(bandDb.begin(), bandDb.end());
    topDb_ = top > topDb_ ? top : topDb_ + 0.02f * (top - topDb_);
    juce::Image::BitmapData px(history_, writeX_, 0, 1, kBands, juce::Image::BitmapData::writeOnly);
    for (int b = 0; b < kBands; ++b) {
        const float t = (bandDb[static_cast<size_t>(b)] - (topDb_ - kRangeDb)) / kRangeDb;
        px.setPixelColour(0, kBands - 1 - b, ui::viz::spectrogram(juce::jlimit(0.0f, 1.0f, t)));
    }
    writeX_ = (writeX_ + 1) % kHistoryColumns;
    ++columnsWritten_;
}

void LiveSpectrogramView::timerCallback()
{
    // Stopped: nothing new arrives, so poll slowly (VIS-7: timers only as
    // fast as needed) and repaint once to grey the picture.
    const bool playing = controller_.playback().isPlaying();
    if (playing != wasPlaying_) {
        wasPlaying_ = playing;
        startTimerHz(playing ? 30 : 4);
        repaint();
    }
    auto* tap = controller_.playback().meterTap();
    if (tap == nullptr) return;
    std::array<float, 2048> l{}, r{};
    int n = 0;
    const double sr = controller_.hasAnalysis() && controller_.currentAnalysis().sample_rate > 0.0
                          ? controller_.currentAnalysis().sample_rate
                          : 48000.0;
    const int before = columnsWritten_;
    while ((n = tap->spectrumRing().pop(l.data(), r.data(), static_cast<int>(l.size()))) > 0)
        pushSamples(l.data(), r.data(), n, sr);
    if (++overlayTick_ % (playing ? 30 : 4) == 0) refreshOverlays(); // findings change slowly
    // A finding tied to a moment names itself while the playhead is in it.
    juce::String now;
    if (playing) {
        const double t = controller_.playback().getPlaybackPosition();
        for (const auto& m : moments_)
            if (t >= m.startSec && t <= m.endSec) now = m.label;
    }
    if (columnsWritten_ != before || now != nowLabel_) {
        nowLabel_ = now;
        repaint();
    }
}

void LiveSpectrogramView::paint(juce::Graphics& g)
{
    g.fillAll(KhensuTheme::bgSurface);
    const auto plot = plotArea();
    // The ring as two blits: oldest (right of the write head) then newest.
    const int w = kHistoryColumns;
    const float leftW = plot.getWidth() * static_cast<float>(w - writeX_) / static_cast<float>(w);
    g.setImageResamplingQuality(juce::Graphics::mediumResamplingQuality);
    const auto p = plot.toNearestInt();
    const int split = juce::roundToInt(leftW);
    g.drawImage(history_, p.getX(), p.getY(), split, p.getHeight(), writeX_, 0, w - writeX_, kBands);
    if (writeX_ > 0)
        g.drawImage(history_, p.getX() + split, p.getY(), p.getWidth() - split, p.getHeight(), 0, 0, writeX_, kBands);

    // Findings as bands across the scroll.
    g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    for (const auto& b : bands_) {
        const float y0 = yForHz(b.highHz), y1 = yForHz(b.lowHz);
        const auto c = severityColour(b.severity);
        g.setColour(c.withAlpha(0.12f));
        g.fillRect(juce::Rectangle<float>(plot.getX(), y0, plot.getWidth(), std::max(2.0f, y1 - y0)));
        g.setColour(c.withAlpha(0.8f));
        g.drawHorizontalLine(juce::roundToInt(y0), plot.getX(), plot.getRight());
        g.drawHorizontalLine(juce::roundToInt(y1), plot.getX(), plot.getRight());
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(b.label, juce::Rectangle<float>(plot.getX() + 6.0f, y0 + 1.0f, plot.getWidth() * 0.6f, 14.0f),
                   juce::Justification::centredLeft, true);
    }
    const bool playing = controller_.playback().isPlaying();
    if (!playing && columnsWritten_ > 0) { // VIS-6: the last picture, greyed
        g.setColour(KhensuTheme::bgSurface.withAlpha(0.45f));
        g.fillRect(plot);
    }
    // The standard frequency axis (VIS-3) and the colour bar (VIS-2).
    ui::viz::paintFrequencyTicks(g, plot, 40.0f, false);
    ui::viz::paintSpectrogramBar(g, juce::Rectangle<float>(plot.getRight() + 6.0f, plot.getY(), ui::viz::kColourBarW - 6.0f,
                                                           plot.getHeight()),
                                 "0 dB", "-60");
    // Header: what this is, and the finding playing right now.
    auto header = getLocalBounds().toFloat().removeFromTop(20.0f).withTrimmedLeft(40.0f);
    g.setColour(KhensuTheme::textSecondary);
    g.drawText(playing ? TRANS("Live - newest on the right")
                       : (columnsWritten_ > 0 ? TRANS("Stopped - the last seconds played; press play to scroll")
                                              : TRANS("Press play: the spectrogram scrolls with the music")),
               header, juce::Justification::centredLeft, true);
    if (nowLabel_.isNotEmpty()) {
        g.setColour(KhensuTheme::accentAmber);
        g.drawText(TRANS("Now:") + " " + nowLabel_, header, juce::Justification::centredRight, true);
    }
}

void LiveSpectrogramView::mouseMove(const juce::MouseEvent& e)
{
    const auto plot = plotArea();
    if (!plot.contains(e.position)) return;
    const float t = (plot.getBottom() - e.position.y) / plot.getHeight();
    const float hz = kLoHz * std::pow(kHiHz / kLoHz, t);
    juce::String tip = ui::viz::freqReadout(hz);
    for (const auto& b : bands_)
        if (hz >= b.lowHz && hz <= b.highHz) tip << " - " << b.label;
    setTooltip(tip);
}

} // namespace khensu
