// desktop-app/Source/ui/shared/MeterBridge.cpp
#include "MeterBridge.h"
#include "PanelRunState.h"
#include "VizMaps.h"
#include "../../IAppController.h"
#include <khensu/targets/GenreDatabase.h>

namespace khensu {

MeterBridge::MeterBridge(IAppController& controller, Layout layout) : controller_(controller), layout_(layout)
{
    setTitle(TRANS("Output meters"));
    loudness_.setCompact(layout_ == Layout::Compact);
    loudness_.setTitle(TRANS("Loudness"));
    loudness_.setDescription(TRANS("Momentary, short-term and integrated loudness of what is playing (LUFS), with the "
                               "genre's target band. Click the integrated value to restart it."));
    loudness_.onResetIntegrated = [this] { resetIntegrated(); };
    truePeak_.setTitle(TRANS("True peak"));
    truePeak_.setDescription(TRANS("True peak of what is playing (dBTP). The line is the -1 dBTP ceiling. Click to "
                               "clear the hold."));
    truePeak_.onReset = [this] { live_.resetPeaks(); };
    correlation_.setTitle(TRANS("Correlation"));
    correlation_.setDescription(TRANS("Phase correlation of what is playing: +1 mono-safe, 0 wide, below 0 cancels "
                                  "in mono."));
    scope_.setTitle(TRANS("Vectorscope"));
    scope_.setDescription(TRANS("The stereo image of what is playing (mid up, side across)."));
    for (juce::Component* c : {(juce::Component*)&loudness_, (juce::Component*)&truePeak_,
                               (juce::Component*)&correlation_})
        if (layout_ == Layout::Compact)
            addChildComponent(c); // compact paints its own readouts
        else
            addAndMakeVisible(c);
    addAndMakeVisible(scope_);
    if (layout_ == Layout::Large) addAndMakeVisible(widthChart_);
    if (layout_ == Layout::Compact) {
        setTooltip(TRANS("Short-term loudness of what is playing against the genre's target band, the held true "
                         "peak and the stereo correlation. Click to restart the readings."));
        setTitle(TRANS("Output meters: loudness, true peak, correlation"));
    }
    // No controller call here (ENG-2): the transport bar that holds one is
    // built in the controller's initializer list. The scope finds the tap
    // when the bridge first shows.
}

MeterBridge::~MeterBridge()
{
    stopTimer();
    scope_.setSource(nullptr);
}

void MeterBridge::visibilityChanged()
{
    // A hidden bridge costs nothing: the meters' vblank stops, and so does
    // the drain (the tap's FIFO then drops its oldest-unread buckets).
    if (isShowing()) {
        if (!scopeConnected_)
            if (auto* tap = controller_.playback().meterTap()) {
                scope_.setSource(layout_ == Layout::Large ? &tap->scopeLargeRing() : &tap->scopeRing());
                scopeConnected_ = true;
            }
        updateTarget();
        updateAnalysedWidth();
        startTimerHz(20);
    } else {
        stopTimer();
    }
}

void MeterBridge::resetIntegrated()
{
    live_.reset();
    truePeak_.resetHold();
    readout_.peakHold = -std::numeric_limits<float>::infinity();
    repaint();
}

void MeterBridge::mouseUp(const juce::MouseEvent& e)
{
    if (layout_ == Layout::Compact && readoutArea_.contains(e.getPosition())) resetIntegrated();
}

void MeterBridge::updateTarget()
{
    // The session genre's LUFS window as the target band (no genre: none).
    float lo = std::numeric_limits<float>::quiet_NaN(), hi = lo;
    if (controller_.hasActiveSession())
        if (auto g = controller_.genreDatabase().findById(controller_.getActiveSession()->target.primary_genre)) {
            lo = g->lufs_range_min;
            hi = g->lufs_range_max;
        }
    readout_.targetLo = lo;
    readout_.targetHi = hi;
    if (std::isnan(lo) || std::isnan(hi) || hi < lo)
        loudness_.setTarget(std::numeric_limits<float>::quiet_NaN());
    else
        loudness_.setTarget(0.5f * (lo + hi), 0.5f * (hi - lo));
    truePeak_.setCeiling(-1.0f);
}

void MeterBridge::timerCallback()
{
    if (layout_ == Layout::Large && ++analysedTick_ % 20 == 0) updateAnalysedWidth(); // a new analysis
    auto* tap = controller_.playback().meterTap();
    if (tap == nullptr) {
        showAnalysedWhileIdle();
        return;
    }
    // A new file (another side, a stem, a new session): a new integration.
    const auto path = controller_.playback().currentAuditionPath();
    if (path != lastAuditionPath_) {
        lastAuditionPath_ = path;
        live_.reset();
        updateTarget();
    }
    int n = 0;
    while ((n = tap->pop(scratch_.data(), (int)scratch_.size())) > 0)
        for (int i = 0; i < n; ++i)
            live_.add(scratch_[(size_t)i]);

    const bool playing = controller_.playback().isPlaying();
    if (playing) setShowingAnalysed(false);
    if (playing && !scopeConnected_) { // back to the live signal
        scope_.setSource(layout_ == Layout::Large ? &tap->scopeLargeRing() : &tap->scopeRing());
        scopeConnected_ = true;
    }
    if (layout_ == Layout::Large) updateWidth(*tap, playing);
    loudness_.setValues(playing ? live_.momentary() : LiveLoudness::kSilence,
                        playing ? live_.shortTerm() : LiveLoudness::kSilence, live_.integrated());
    truePeak_.setPeaks(live_.truePeakDb(0), live_.truePeakDb(1));
    if (layout_ == Layout::Compact) {
        Readout next = readout_;
        next.shortTerm = playing ? live_.shortTerm() : LiveLoudness::kSilence;
        next.integrated = live_.integrated();
        next.peakHold = std::max({readout_.peakHold, live_.truePeakDb(0), live_.truePeakDb(1)});
        next.correlation = live_.correlation();
        const auto moved = [](float a, float b) {
            return std::isfinite(a) != std::isfinite(b) || (std::isfinite(a) && std::abs(a - b) > 0.05f);
        };
        const bool changed = moved(next.shortTerm, readout_.shortTerm) || moved(next.integrated, readout_.integrated)
                             || moved(next.peakHold, readout_.peakHold) || moved(next.correlation, readout_.correlation);
        readout_ = next;
        if (changed) repaint(readoutArea_);
    }
    live_.resetPeaks(); // each push carries the peak since the last one; the meter holds
    correlation_.setCorrelation(live_.correlation());
    if (!playing) showAnalysedWhileIdle();
}

void MeterBridge::showAnalysedWhileIdle()
{
    // Nothing is playing: the meters show what the analysis measured instead
    // of sitting blank (the card is the song's meters, not only a live view).
    if (!controller_.hasAnalysis()) {
        setShowingAnalysed(false);
        return;
    }
    setShowingAnalysed(true); // VIS-6: greyed, with the caption
    const auto& r = controller_.currentAnalysis();
    const float lufs = r.loudness.lufs_integrated;
    const bool haveLufs = r.has_fast_analysis() && lufs > -70.0f;
    // The S bar holds the song's loudest short-term reading, the M bar is
    // empty (there is no momentary reading of a stopped song).
    const float stMax = r.loudness.lufs_short_term_max > -70.0f ? r.loudness.lufs_short_term_max : LiveLoudness::kSilence;
    loudness_.setValues(LiveLoudness::kSilence, haveLufs ? stMax : LiveLoudness::kSilence,
                        haveLufs ? lufs : LiveLoudness::kSilence);
    if (r.loudness.lra_measured) loudness_.setLoudnessRange(r.loudness.lra);
    if (r.loudness.true_peak_dbtp > -98.0f) truePeak_.setPeaks(r.loudness.true_peak_dbtp, r.loudness.true_peak_dbtp);
    correlation_.setCorrelation(r.stereo.correlation);
    if (layout_ == Layout::Compact) {
        Readout next = readout_;
        next.shortTerm = haveLufs ? stMax : LiveLoudness::kSilence;
        next.integrated = haveLufs ? lufs : LiveLoudness::kSilence;
        if (r.loudness.true_peak_dbtp > -98.0f) next.peakHold = r.loudness.true_peak_dbtp;
        next.correlation = r.stereo.correlation;
        readout_ = next;
        repaint(readoutArea_);
    }
    // The vectorscope (both layouts, VIS-6): a slice of the song itself,
    // looped and greyed, so its stereo image shows without playback. Decoded
    // once per file, in the background.
    const auto path = controller_.currentAuditionPath().isNotEmpty() ? controller_.currentAuditionPath()
                                                                     : controller_.activeSourcePath();
    if (path != idleScopeFor_ && !idleScopeLoading_) {
        idleScopeFor_ = path;
        idleScopeLoading_ = true;
        idleLeft_.clear();
        idleRight_.clear();
        fetchQuietly(
            *this, controller_,
            [path] {
                std::pair<std::vector<float>, std::vector<float>> out;
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(path)));
                if (reader == nullptr || reader->lengthInSamples <= 0) return out;
                const auto n = static_cast<int>(std::min<juce::int64>(reader->lengthInSamples, (juce::int64)(reader->sampleRate * 8.0)));
                const auto start = std::max<juce::int64>(0, reader->lengthInSamples / 2 - n / 2); // the middle 8 s
                juce::AudioBuffer<float> buf(2, n);
                reader->read(&buf, 0, n, start, true, true);
                out.first.assign(buf.getReadPointer(0), buf.getReadPointer(0) + n);
                out.second.assign(buf.getReadPointer(reader->numChannels > 1 ? 1 : 0),
                                  buf.getReadPointer(reader->numChannels > 1 ? 1 : 0) + n);
                return out;
            },
            [](MeterBridge& self, std::pair<std::vector<float>, std::vector<float>> lr) {
                self.idleLeft_ = std::move(lr.first);
                self.idleRight_ = std::move(lr.second);
                self.idleScopePos_ = 0;
            },
            [](MeterBridge& self) { self.idleScopeLoading_ = false; });
    }
    if (!idleLeft_.empty()) {
        if (scopeConnected_) {
            scope_.setSource(nullptr); // stopped: show the looped slice
            scopeConnected_ = false;
        }
        const int chunk = 2205; // ~50 ms at 44.1 kHz per 20 Hz tick
        if (idleScopePos_ + chunk > idleLeft_.size()) idleScopePos_ = 0;
        scope_.pushSamples(idleLeft_.data() + idleScopePos_, idleRight_.data() + idleScopePos_, chunk);
        idleScopePos_ += chunk;
    }
}

void MeterBridge::setShowingAnalysed(bool analysed)
{
    if (analysed == showingAnalysed_) return;
    showingAnalysed_ = analysed;
    for (ui::AnimatedMeter* m : {static_cast<ui::AnimatedMeter*>(&loudness_), static_cast<ui::AnimatedMeter*>(&truePeak_),
                                 static_cast<ui::AnimatedMeter*>(&correlation_), static_cast<ui::AnimatedMeter*>(&scope_)})
        m->setStopped(analysed);
    widthChart_.setStopped(analysed);
    setDescription(analysed ? ui::viz::stoppedCaptionText() : juce::String());
    repaint();
}

void MeterBridge::paintCompact(juce::Graphics& g)
{
    // Three readouts, label over value: loudness (with a bar against the
    // target band), true peak, correlation. Numbers in tabular figures.
    auto area = readoutArea_;
    if (area.isEmpty()) return;
    const int colGap = KhensuTheme::sp3;
    const int loudW = juce::jmax(120, area.getWidth() - 2 * (72 + colGap));
    auto loud = area.removeFromLeft(loudW);
    area.removeFromLeft(colGap);
    auto peak = area.removeFromLeft(72);
    area.removeFromLeft(colGap);
    auto corr = area;

    const auto label = [&g](juce::Rectangle<int> r, const juce::String& text) {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
        g.drawText(text, r, juce::Justification::centredLeft, true);
    };
    const bool stopped = showingAnalysed_;
    const auto value = [&g, stopped](juce::Rectangle<int> r, const juce::String& text, juce::Colour colour) {
        g.setColour(stopped ? ui::viz::stoppedTint(colour) : colour); // VIS-6: analysed, not live
        g.setFont(KhensuTheme::metricFont(KhensuTheme::kFontBody + 2.0f));
        g.drawText(text, r, juce::Justification::centredLeft, true);
    };
    const juce::String dash = juce::String(juce::CharPointer_UTF8("\xe2\x80\x94")); // utf8-exempt(em dash)
    const auto lufsText = [&dash](float v) {
        return v > LiveLoudness::kSilence + 1.0f && std::isfinite(v) ? juce::String(v, 1) : dash;
    };
    const int top = loud.getY() + juce::jmax(0, (loud.getHeight() - 36) / 2);

    // Loudness.
    {
        auto r = loud.withY(top).withHeight(36);
        label(r.removeFromTop(14), (stopped ? TRANS("Stopped - analysed LUFS") : TRANS("Short-term LUFS")) + "   "
                                       + TRANS("int") + " " + lufsText(readout_.integrated));
        auto row = r;
        value(row.removeFromLeft(48), lufsText(readout_.shortTerm), KhensuTheme::textPrimary);
        auto bar = row.reduced(0, 9).toFloat();
        const float lo = -36.0f, hi = -4.0f;
        const auto x = [&bar, lo, hi](float v) {
            return bar.getX() + bar.getWidth() * juce::jlimit(0.0f, 1.0f, (v - lo) / (hi - lo));
        };
        g.setColour(KhensuTheme::borderSubtle);
        g.fillRoundedRectangle(bar, 2.0f);
        if (std::isfinite(readout_.targetLo) && std::isfinite(readout_.targetHi) && readout_.targetHi >= readout_.targetLo) {
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.25f));
            g.fillRect(juce::Rectangle<float>(x(readout_.targetLo), bar.getY() - 2.0f,
                                              juce::jmax(2.0f, x(readout_.targetHi) - x(readout_.targetLo)),
                                              bar.getHeight() + 4.0f));
        }
        if (readout_.shortTerm > lo) {
            g.setColour(stopped ? ui::viz::stoppedTint(KhensuTheme::accentTeal) : KhensuTheme::accentTeal);
            g.fillRoundedRectangle(bar.withRight(x(readout_.shortTerm)), 2.0f);
        }
    }
    // True peak (held; red over the -1 dBTP ceiling).
    {
        auto r = peak.withY(top).withHeight(36);
        label(r.removeFromTop(14), TRANS("True peak"));
        const bool known = std::isfinite(readout_.peakHold) && readout_.peakHold > -99.0f;
        value(r, known ? juce::String(readout_.peakHold, 1) : dash,
              known && readout_.peakHold > -1.0f ? KhensuTheme::accentRed : KhensuTheme::textPrimary);
    }
    // Correlation (amber below 0, red below -0.5).
    {
        auto r = corr.withY(top).withHeight(36);
        label(r.removeFromTop(14), TRANS("Correlation"));
        const float c = readout_.correlation;
        value(r, (c >= 0.0f ? "+" : "") + juce::String(c, 2),
              c < -0.5f ? KhensuTheme::accentRed : c < 0.0f ? KhensuTheme::accentAmber : KhensuTheme::textPrimary);
    }
}

void MeterBridge::paint(juce::Graphics& g)
{
    if (layout_ == Layout::Compact) {
        paintCompact(g);
        return;
    }
    if (layout_ == Layout::Large) {
        g.setColour(KhensuTheme::bgSurface);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
        g.setColour(KhensuTheme::borderSubtle);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 8.0f, 1.0f);
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
        g.drawText(TRANS("Output meters"), getLocalBounds().reduced(KhensuTheme::sp4, KhensuTheme::sp2).removeFromTop(18),
                   juce::Justification::centredLeft, false);
    }
}

void MeterBridge::paintOverChildren(juce::Graphics& g)
{
    if (layout_ != Layout::Large || !showingAnalysed_) return;
    // VIS-6: one caption for the card, on its title row.
    auto row = getLocalBounds().reduced(KhensuTheme::sp4, KhensuTheme::sp2).removeFromTop(18).toFloat();
    ui::viz::paintStoppedCaption(g, row.withTrimmedTop(2.0f));
}

void MeterBridge::updateWidth(PlaybackMeterTap& tap, bool playing)
{
    constexpr int kWindow = 4096;
    // Drain the width ring every tick (it holds ~0.3 s) and keep the newest
    // kWindow samples.
    std::array<float, 2048> l{}, r{};
    int n = 0;
    while ((n = tap.widthRing().pop(l.data(), r.data(), static_cast<int>(l.size()))) > 0) {
        widthLeft_.insert(widthLeft_.end(), l.begin(), l.begin() + n);
        widthRight_.insert(widthRight_.end(), r.begin(), r.begin() + n);
    }
    if (widthLeft_.size() > static_cast<size_t>(kWindow)) {
        const auto drop = widthLeft_.size() - static_cast<size_t>(kWindow);
        widthLeft_.erase(widthLeft_.begin(), widthLeft_.begin() + static_cast<long>(drop));
        widthRight_.erase(widthRight_.begin(), widthRight_.begin() + static_cast<long>(drop));
    }
    if (++widthTick_ % 4 != 0) return; // 5 updates a second is plenty for a bar chart
    const bool analysed = controller_.hasAnalysis();
    updateAnalysedWidth();
    const double sr = analysed && controller_.currentAnalysis().sample_rate > 0.0 ? controller_.currentAnalysis().sample_rate
                                                                                    : 48000.0;
    if (playing && widthLeft_.size() >= static_cast<size_t>(kWindow))
        widthChart_.setLive(width_bands::fromSamples(widthLeft_.data(), widthRight_.data(), kWindow, sr));
    else if (!playing)
        widthChart_.setLive({});
}

void MeterBridge::updateAnalysedWidth()
{
    if (layout_ != Layout::Large) return;
    // The analysed bars and the genre window show without playback too.
    if (controller_.hasAnalysis())
        widthChart_.setAnalysed(width_bands::fromOctaveLevels(controller_.currentAnalysis().spectral.rms_mid_per_octave_db,
                                                              controller_.currentAnalysis().spectral.rms_side_per_octave_db));
    else
        widthChart_.setAnalysed({});
    std::optional<std::pair<float, float>> window;
    if (controller_.hasActiveSession())
        if (auto g = controller_.genreDatabase().findById(controller_.getActiveSession()->target.primary_genre))
            window = std::make_pair(g->stereo_width_min, g->stereo_width_max);
    widthChart_.setWindow(window);
}

void MeterBridge::resized()
{
    auto b = getLocalBounds();
    if (layout_ == Layout::Compact) {
        // The live scope at the right, the painted readouts beside it.
        const int h = b.getHeight();
        scope_.setBounds(b.removeFromRight(h).reduced(2));
        b.removeFromRight(KhensuTheme::sp3);
        readoutArea_ = b;
        return;
    }
    b = b.reduced(KhensuTheme::sp4);
    b.removeFromTop(22); // the card's caption
    // VIZ-4b: the per-octave width under the meters, so the card is full.
    widthChart_.setBounds(b.removeFromBottom(juce::jlimit(90, 180, b.getHeight() * 3 / 10)));
    b.removeFromBottom(KhensuTheme::sp3);

    // BUG-5: every meter gets a box it can draw in without its captions or
    // scales colliding. The correlation meter needs ~44 px (caption, track,
    // scale); the loudness meter ~170 px of width for readable bars and ticks.
    constexpr int kCorrH = 44, kPeakW = 64, kLoudMinW = 170;
    const int gap = KhensuTheme::sp3;
    const int oneRowScope = juce::jmin(b.getHeight() - kCorrH - KhensuTheme::sp1, b.getWidth() * 2 / 5);
    if (b.getWidth() - oneRowScope - kPeakW - 2 * gap >= kLoudMinW && oneRowScope >= 100) {
        // Wide: [scope over correlation] [loudness] [true peak].
        auto left = b.removeFromLeft(oneRowScope);
        scope_.setBounds(left.removeFromTop(oneRowScope));
        left.removeFromTop(KhensuTheme::sp1);
        correlation_.setBounds(left.removeFromTop(kCorrH));
        b.removeFromLeft(gap);
        truePeak_.setBounds(b.removeFromRight(kPeakW));
        b.removeFromRight(gap);
        loudness_.setBounds(b);
        return;
    }
    // Narrow: [loudness | true peak] over [scope | correlation].
    auto top = b.removeFromTop(juce::jlimit(84, 120, b.getHeight() / 2));
    truePeak_.setBounds(top.removeFromRight(kPeakW));
    top.removeFromRight(gap);
    loudness_.setBounds(top);
    b.removeFromTop(gap);
    const int scopeSize = juce::jmax(0, juce::jmin(b.getHeight(), b.getWidth() / 2));
    scope_.setBounds(b.removeFromLeft(scopeSize).withSizeKeepingCentre(scopeSize, scopeSize));
    b.removeFromLeft(gap);
    correlation_.setBounds(b.withSizeKeepingCentre(b.getWidth(), juce::jmin(kCorrH, b.getHeight())));
}

} // namespace khensu
