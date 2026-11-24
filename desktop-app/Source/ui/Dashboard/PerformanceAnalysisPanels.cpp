// desktop-app/Source/ui/Dashboard/PerformanceAnalysisPanels.cpp
#include "PerformanceAnalysisPanels.h"
#include "../shared/SessionAudioPaths.h"
#include <cmath>

namespace khensu {

namespace ft = feature_text;

namespace {

/// The session's source mix, or "" (and a status line) without one.
juce::String sourceOrSay(IAppController& app, juce::Label& status)
{
    const juce::String path = app.activeSourcePath();
    if (path.isEmpty()) status.setText(TRANS("No active session."), juce::dontSendNotification);
    return path;
}

void drawPlotFrame(juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
    g.fillRoundedRectangle(r, 4.0f);
}

} // namespace

// ---- Groove timing ---------------------------------------------------------

GrooveTimingPanel::GrooveTimingPanel(IAppController& c)
    : OnDemandScanPanel(c, "Groove Timing", TRANS("Analyze Groove"),
                        TRANS("Measure swing, pocket and timing spread against the beat grid."))
{
    setExplainer(TRANS("Groove Timing"),
                 TRANS("Where the hits sit against the tracked beat grid (8th, 16th or triplet - chosen from where "
                       "the onsets actually fall).\n\nSwing % is the long note's share of each off-beat pair: 50% is "
                       "straight, about 67% is triplet swing. Pocket is the median offset from the grid: negative "
                       "pushes (ahead), positive lays back (behind). Spread is how much the hits scatter around "
                       "their own pocket - under ~8 ms reads as quantised/tight, 8-18 ms as a human feel, more as "
                       "loose.\n\nThe plot shows each onset's offset over time (up = late, down = early)."));
    statusLabel_.setText(idleText(), juce::dontSendNotification);
}

juce::String GrooveTimingPanel::idleText() const { return TRANS("Measures swing, pocket and timing spread."); }

void GrooveTimingPanel::clearResult() { result_ = {}; }

void GrooveTimingPanel::layoutBody(juce::Rectangle<int> area)
{
    textArea_ = area.removeFromTop(80);
    area.removeFromTop(4);
    plotArea_ = area;
}

void GrooveTimingPanel::run()
{
    const auto path = sourceOrSay(app_, statusLabel_);
    if (path.isEmpty()) return;
    startScan(
        TRANS("Tracking the beat grid..."),
        [path] { return ipc::HttpServiceBridge{}.analyzeGrooveTiming(path); },
        [this](const ipc::HttpServiceBridge::GrooveTimingResult& r) {
            result_ = r;
            statusLabel_.setText(r.pocket == "insufficient_onsets"
                                     ? TRANS("Too few clear onsets to measure.")
                                     : TRANS("Groove measured on") + " " + juce::String((int)r.events.size()) + " " +
                                           TRANS("onsets."),
                                 juce::dontSendNotification);
        },
        /*longPool=*/true);
}

void GrooveTimingPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!showsResult()) return;
    const auto text = textArea_;
    g.setFont(KhensuTheme::labelFont(10.5f));
    g.setColour(ft::spreadTone(result_.timingStdMs) == ft::Tone::Moderate ? KhensuTheme::accentAmber
                                                                          : KhensuTheme::textPrimary);
    g.drawFittedText(ft::grooveSummary(result_), text, juce::Justification::topLeft, 6, 1.0f);

    if (result_.events.empty() || plotArea_.getHeight() < 30) return;
    const auto r = plotArea_.toFloat();
    drawPlotFrame(g, r);
    // +-half a 16th at the tempo, clamped to a readable 10..60 ms range.
    const double halfCellMs =
        result_.bpm > 0.0 ? juce::jlimit(10.0, 60.0, 60000.0 / result_.bpm / 8.0) : 40.0;
    const float midY = r.getCentreY();
    g.setColour(KhensuTheme::textTertiary.withAlpha(0.35f));
    g.drawHorizontalLine((int)midY, r.getX(), r.getRight());
    const float tol = (float)(12.0 / halfCellMs) * (r.getHeight() * 0.5f);
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.08f));
    g.fillRect(r.getX(), midY - tol, r.getWidth(), 2.0f * tol); // the +-12 ms "on grid" band

    double span = 0.0;
    for (const auto& e : result_.events)
        span = std::max(span, e.timeSec);
    if (span <= 0.0) span = 1.0;
    for (const auto& e : result_.events) {
        const float x = r.getX() + ft::timelineX(e.timeSec, span, r.getWidth());
        const float yNorm = (float)juce::jlimit(-1.0, 1.0, e.offsetMs / halfCellMs);
        const float y = midY + yNorm * (r.getHeight() * 0.5f - 2.0f);
        g.setColour(std::abs(e.offsetMs) <= 12.0 ? KhensuTheme::accentTeal
                    : e.offsetMs < 0.0           ? KhensuTheme::accentBlue
                                                 : KhensuTheme::accentAmber);
        g.fillEllipse(x - 2.0f, y - 2.0f, 4.0f, 4.0f);
    }
    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::labelFont(8.0f));
    g.drawText(TRANS("late"), r.withHeight(10.0f).reduced(4.0f, 0.0f), juce::Justification::centredRight, false);
    g.drawText(TRANS("early"), r.withTop(r.getBottom() - 10.0f).reduced(4.0f, 0.0f),
               juce::Justification::centredRight, false);
}

// ---- Harmony timeline ------------------------------------------------------

HarmonyTimelinePanel::HarmonyTimelinePanel(IAppController& c)
    : OnDemandScanPanel(c, "Harmony Timeline", TRANS("Map Harmony"),
                        TRANS("Track the chord progression and key changes over the whole song."))
{
    setExplainer(TRANS("Harmony Timeline"),
                 TRANS("The chord progression over time, with the key in force and every sustained key change "
                       "(modulation) marked. Chords use the same detector as the Chords card; a key change is only "
                       "reported after several windows agree, so a I-IV-V loop does not read as modulating.\n\n"
                       "Each block is one chord, as long as it lasts; brighter blocks are more confident. "
                       "Vertical markers are key changes."));
    statusLabel_.setText(idleText(), juce::dontSendNotification);
}

juce::String HarmonyTimelinePanel::idleText() const { return TRANS("Maps chords and key changes over time."); }

void HarmonyTimelinePanel::clearResult()
{
    result_ = {};
    spanSec_ = 0.0;
}

void HarmonyTimelinePanel::layoutBody(juce::Rectangle<int> area) { stripArea_ = area; }

void HarmonyTimelinePanel::run()
{
    const auto path = sourceOrSay(app_, statusLabel_);
    if (path.isEmpty()) return;
    const double duration = app_.playback().getPlaybackDuration();
    startScan(
        TRANS("Tracking chords and key..."),
        [path] { return ipc::HttpServiceBridge{}.analyzeHarmonyTimeline(path); },
        [this, duration](const ipc::HttpServiceBridge::HarmonyTimelineResult& r) {
            result_ = r;
            spanSec_ = ft::timelineSpan(r, duration);
            statusLabel_.setText(ft::harmonySummary(r), juce::dontSendNotification);
        },
        /*longPool=*/true);
}

void HarmonyTimelinePanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!showsResult() || result_.progression.empty() || stripArea_.getHeight() < 40) return;

    auto area = stripArea_;
    auto keyRow = area.removeFromTop(16);
    auto strip = area.removeFromTop(juce::jmin(46, area.getHeight())).toFloat();
    area.removeFromTop(4);
    drawPlotFrame(g, strip);
    const float w = strip.getWidth();

    // Chord blocks: each lasts until the next chord (the last to the end).
    g.setFont(KhensuTheme::labelFont(9.5f));
    for (size_t i = 0; i < result_.progression.size(); ++i) {
        const auto& c = result_.progression[i];
        const double end = i + 1 < result_.progression.size() ? result_.progression[i + 1].timeSec : spanSec_;
        const float x0 = strip.getX() + ft::timelineX(c.timeSec, spanSec_, w);
        const float x1 = strip.getX() + ft::timelineX(end, spanSec_, w);
        const auto block = juce::Rectangle<float>(x0, strip.getY() + 3.0f, juce::jmax(1.0f, x1 - x0 - 1.0f),
                                                  strip.getHeight() - 6.0f);
        const float alpha = 0.18f + 0.5f * (float)juce::jlimit(0.0, 1.0, c.confidence);
        g.setColour(KhensuTheme::accentTeal.withAlpha(i % 2 == 0 ? alpha : alpha * 0.7f)); // UX2-3: one accent
        g.fillRoundedRectangle(block, 2.0f);
        if (block.getWidth() > 18.0f) {
            g.setColour(KhensuTheme::textPrimary);
            g.drawText(c.chord, block.reduced(2.0f, 0.0f), juce::Justification::centred, true);
        }
    }

    // Key: the initial key at the left, a marker + label at each change.
    g.setFont(KhensuTheme::labelFont(9.0f).boldened());
    if (result_.initialKey.has_value()) {
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(ft::keyName(*result_.initialKey), keyRow.withTrimmedLeft(2), juce::Justification::centredLeft,
                   false);
    }
    for (const auto& k : result_.keyChanges) {
        const float x = strip.getX() + ft::timelineX(k.timeSec, spanSec_, w);
        g.setColour(KhensuTheme::textPrimary); // a key change is an event, not a warning
        g.fillRect(x - 1.0f, strip.getY(), 2.0f, strip.getHeight());
        g.drawText(ft::keyName(k), juce::Rectangle<float>(x + 3.0f, (float)keyRow.getY(), 90.0f, 16.0f),
                   juce::Justification::centredLeft, false);
    }

    // Time axis + the progression as text below the strip.
    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::labelFont(8.0f));
    auto axis = area.removeFromTop(12);
    const int ticks = 4;
    for (int t = 0; t <= ticks; ++t) {
        const double sec = spanSec_ * t / ticks;
        const int x = axis.getX() + (int)ft::timelineX(sec, spanSec_, (float)axis.getWidth());
        const juce::String label = juce::String((int)sec / 60) + ":" + juce::String((int)sec % 60).paddedLeft('0', 2);
        g.drawText(label, juce::Rectangle<int>(juce::jlimit(axis.getX(), axis.getRight() - 30, x - 15), axis.getY(), 30, 12),
                   juce::Justification::centred, false);
    }
    if (area.getHeight() > 12) {
        juce::StringArray chords;
        for (const auto& c : result_.progression)
            chords.add(c.chord);
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::labelFont(9.5f));
        g.drawFittedText(chords.joinIntoString(" - "), area, juce::Justification::topLeft,
                         juce::jmax(1, area.getHeight() / 12), 1.0f);
    }
}

// ---- Reverb tail -----------------------------------------------------------

ReverbTailPanel::ReverbTailPanel(IAppController& c)
    : OnDemandScanPanel(c, "Reverb Tail", TRANS("Measure Tail"),
                        TRANS("Estimate the reverb decay time (RT60) and how much tail sits between hits."))
{
    setExplainer(TRANS("Reverb Tail"),
                 TRANS("RT60 is how long the sound takes to decay by 60 dB. It is measured only where the audio "
                       "decays freely - stops, endings and isolated hits - and the median of those decays is shown "
                       "(dense material with no gaps has no measurable RT60, and says so).\n\n"
                       "The tail ratio is the energy between hits relative to the hits themselves: a high ratio "
                       "means a wet, washy mix where the space may mask detail; a low one means dry and upfront. "
                       "Use it with the Depth Ladder to judge front-to-back staging."));
    statusLabel_.setText(idleText(), juce::dontSendNotification);
}

juce::String ReverbTailPanel::idleText() const { return TRANS("Measures RT60 and the tail between hits."); }

void ReverbTailPanel::clearResult() { result_ = {}; }

void ReverbTailPanel::layoutBody(juce::Rectangle<int> area) { readoutArea_ = area; }

void ReverbTailPanel::run()
{
    const auto path = sourceOrSay(app_, statusLabel_);
    if (path.isEmpty()) return;
    startScan(
        TRANS("Finding free decays..."), [path] { return ipc::HttpServiceBridge{}.analyzeReverbTail(path); },
        [this](const ipc::HttpServiceBridge::ReverbTailResult& r) {
            result_ = r;
            statusLabel_.setText(r.rt60Sec.has_value()
                                     ? TRANS("Measured from") + " " + juce::String(r.decaySegments) + " " +
                                           TRANS("free-decay segment(s).")
                                     : TRANS("No usable free decay found."),
                                 juce::dontSendNotification);
        },
        /*longPool=*/true);
}

void ReverbTailPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!showsResult()) return;
    auto b = readoutArea_;

    // Big RT60 readout + room character.
    auto top = b.removeFromTop(34);
    g.setColour(result_.rt60Sec.has_value() ? KhensuTheme::textPrimary : KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::metricFont(20.0f));
    g.drawText(ft::rt60Text(result_.rt60Sec), top.removeFromLeft(top.getWidth() / 2), juce::Justification::centredLeft,
               true);
    g.setFont(KhensuTheme::labelFont(10.5f));
    g.setColour(KhensuTheme::textSecondary);
    juce::String detail = ft::spaceCharacter(result_.rt60Sec);
    if (result_.rt60Sec.has_value()) detail << "  (" << TRANS("fit") << " R2 " << juce::String(result_.fitR2, 2) << ")";
    g.drawText(detail, top, juce::Justification::centredLeft, true);
    b.removeFromTop(6);

    // Tail ratio bar, 0..0.6 scale, with the 0.15 / 0.35 label thresholds.
    auto label = b.removeFromTop(14);
    g.setColour(ft::tailTone(result_.tailLabel) == ft::Tone::Moderate ? KhensuTheme::accentAmber
                                                                      : KhensuTheme::textPrimary);
    g.setFont(KhensuTheme::labelFont(10.0f));
    g.drawText(ft::tailLabelText(result_.tailLabel) + "  (" + TRANS("tail ratio") + " " +
                   juce::String(result_.tailEnergyRatio, 2) + ")",
               label, juce::Justification::centredLeft, true);
    auto bar = b.removeFromTop(10).toFloat();
    constexpr double kScale = 0.6;
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
    g.fillRoundedRectangle(bar, 3.0f);
    g.setColour(OnDemandScanPanel::toneColour(ft::tailTone(result_.tailLabel)));
    g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * (float)juce::jlimit(0.0, 1.0, result_.tailEnergyRatio / kScale)),
                           3.0f);
    g.setColour(KhensuTheme::textTertiary.withAlpha(0.6f));
    for (double t : {0.15, 0.35})
        g.fillRect(bar.getX() + bar.getWidth() * (float)(t / kScale), bar.getY() - 2.0f, 1.0f, bar.getHeight() + 4.0f);
    b.removeFromTop(8);

    if (result_.note.isNotEmpty()) {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(9.5f));
        g.drawFittedText(result_.note, b, juce::Justification::topLeft, juce::jmax(1, b.getHeight() / 12), 1.0f);
    }
}

// ---- Stem phase ------------------------------------------------------------

StemPhasePanel::StemPhasePanel(IAppController& c)
    : OnDemandScanPanel(c, "Stem Phase", TRANS("Check Stem Phase"),
                        TRANS("Correlate every pair of separated stems to find polarity flips and shared bleed."))
{
    setExplainer(TRANS("Stem Phase"),
                 TRANS("Broadband correlation between every pair of separated stems. Independent parts sit near 0 "
                       "(normal). A strongly NEGATIVE pair (<= -0.65) is inverted: the two partly cancel when "
                       "summed, the classic flipped-polarity or misaligned-mic problem. A strongly POSITIVE pair "
                       "(>= +0.65) shares content - bleed or a doubled part.\n\nNeeds AI-separated (Demucs) stems; "
                       "the approximate harmonic/percussive split is not a source separation, so it is not "
                       "checked."));
    statusLabel_.setText(idleText(), juce::dontSendNotification);
    refreshAvailability();
}

bool StemPhasePanel::canRun() const { return app_.stems().hasStemsAvailable(); }

juce::String StemPhasePanel::idleText() const
{
    if (app_.stems().hasStemsAvailable()) return TRANS("Ready - compares every pair of stems.");
    if (app_.stems().hasApproximateStems())
        return TRANS("Needs AI-separated stems - the current stems are an approximate split.");
    return TRANS("Needs separated stems - run Isolate Stems first.");
}

void StemPhasePanel::refreshAvailability()
{
    OnDemandScanPanel::refreshAvailability();
    if (!isBusy() && !showsResult() && !run_.hasError())
        statusLabel_.setText(idleText(), juce::dontSendNotification);
}

void StemPhasePanel::clearResult() { result_ = {}; }

void StemPhasePanel::layoutBody(juce::Rectangle<int> area) { rowsArea_ = area; }

void StemPhasePanel::run()
{
    if (!canRun()) {
        statusLabel_.setText(idleText(), juce::dontSendNotification);
        return;
    }
    std::map<juce::String, juce::String> stems;
    for (const auto& [name, path] : session_audio::activeStemPaths(app_))
        stems[name] = path;
    if (stems.size() < 2) {
        statusLabel_.setText(TRANS("At least two stems are required."), juce::dontSendNotification);
        return;
    }
    startScan(
        TRANS("Correlating stems..."), [stems] { return ipc::HttpServiceBridge{}.analyzeStemPhase(stems); },
        [this](const ipc::HttpServiceBridge::StemPhaseResult& r) {
            result_ = r;
            ft::sortStemPairs(result_.pairs);
            int flagged = 0;
            for (const auto& p : result_.pairs)
                if (p.polarity == "inverted") ++flagged;
            statusLabel_.setText(flagged > 0 ? juce::String(flagged) + " " + TRANS("inverted pair(s) - see below.")
                                             : TRANS("No inverted stem pairs."),
                                 juce::dontSendNotification);
        },
        /*longPool=*/true);
}

void StemPhasePanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!showsResult()) return;
    auto b = rowsArea_;
    constexpr int kRowH = 18;
    g.setFont(KhensuTheme::labelFont(10.0f));
    const int maxRows = juce::jmax(0, b.getHeight() / kRowH);
    int shown = 0;
    for (const auto& p : result_.pairs) {
        if (shown == maxRows) break;
        auto row = b.removeFromTop(kRowH);
        // A centred correlation bar (-1..+1) then the explanation.
        auto barArea = row.removeFromLeft(70).reduced(2, 5).toFloat();
        g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
        g.fillRect(barArea);
        const float mid = barArea.getCentreX();
        const float len = (float)juce::jlimit(-1.0, 1.0, p.correlation) * barArea.getWidth() * 0.5f;
        g.setColour(OnDemandScanPanel::toneColour(ft::stemPairTone(p.polarity)));
        g.fillRect(juce::Rectangle<float>(len >= 0.0f ? mid : mid + len, barArea.getY(), std::abs(len),
                                          barArea.getHeight()));
        g.setColour(KhensuTheme::textTertiary);
        g.fillRect(mid, barArea.getY() - 1.0f, 1.0f, barArea.getHeight() + 2.0f);
        g.setColour(p.polarity == "inverted" ? KhensuTheme::accentRed : KhensuTheme::textPrimary);
        g.drawText(ft::stemPairLine(p), row.withTrimmedLeft(4), juce::Justification::centredLeft, true);
        ++shown;
    }
    if (shown < (int)result_.pairs.size()) {
        g.setColour(KhensuTheme::textTertiary);
        g.drawText("+" + juce::String((int)result_.pairs.size() - shown) + " " + TRANS("more pair(s)"),
                   rowsArea_.withTop(rowsArea_.getBottom() - 12), juce::Justification::centredRight, false);
    }
}

} // namespace khensu
