// desktop-app/Source/ui/shared/SessionHealthStrip.cpp
#include "SessionHealthStrip.h"
#include "../../IAppController.h"
#include "KhensuLookAndFeel.h"
#include "KhensuTheme.h"

namespace khensu {

SessionHealthStrip::SessionHealthStrip(IAppController& c) : controller_(c)
{
    const auto now = juce::Time::currentTimeMillis();
    sessionStartMs_ = now;
    lastBreakEndMs_ = now;
    lastDecisionWindowMs_ = now;
    setInterceptsMouseClicks(false, false);
    startTimer(5000); // 5 s cadence is plenty for minute-scale state
}

SessionHealthStrip::~SessionHealthStrip() { stopTimer(); }

void SessionHealthStrip::timerCallback()
{
    const auto now = juce::Time::currentTimeMillis();

    // ---- Break detection: a pause >= 3 minutes counts as a real break. ----
    if (controller_.playback().isPlaying()) {
        if (pauseStartedMs_ != 0) {
            if (now - pauseStartedMs_ >= 3 * 60 * 1000) lastBreakEndMs_ = now; // break just ended
            pauseStartedMs_ = 0;
        }
    } else if (pauseStartedMs_ == 0) {
        pauseStartedMs_ = now;
    }

    // ---- Decision rate: applied renders per rolling 10-minute window. ----
    const int renders = (int)controller_.renders().getAppliedRenderHistory().size();
    if (renders > rendersAtLastTick_) decisionsThisWindow_ += renders - rendersAtLastTick_;
    rendersAtLastTick_ = renders;
    if (now - lastDecisionWindowMs_ >= 10 * 60 * 1000) {
        decisionsPer10Min_ = (float)decisionsThisWindow_;
        decisionsThisWindow_ = 0;
        lastDecisionWindowMs_ = now;
    }

    repaint();
}

void SessionHealthStrip::paint(juce::Graphics& g)
{
    const auto now = juce::Time::currentTimeMillis();
    const int sessionMin = (int)((now - sessionStartMs_) / 60000);
    // While a qualifying pause is running, the ears are already resting -
    // count the rest as break time rather than work time.
    const bool onBreakNow = pauseStartedMs_ != 0 && (now - pauseStartedMs_) >= 3 * 60 * 1000;
    const int sinceBreakMin = onBreakNow ? 0 : (int)((now - lastBreakEndMs_) / 60000);

    // Ear-fatigue estimate: continuous listening time weighted by monitoring
    // level (full volume wears roughly twice as fast as -12 dB-ish listening).
    const float vol = controller_.playback().getPlaybackVolume(); // 0..1
    const float levelWeight = 0.6f + 0.8f * vol;
    const float fatigue01 = juce::jlimit(0.0f, 1.0f, (sinceBreakMin * levelWeight) / 90.0f);

    juce::Colour fatigueCol = fatigue01 < 0.5f   ? KhensuTheme::accentTeal
                              : fatigue01 < 0.8f ? KhensuTheme::accentAmber
                                                 : KhensuTheme::accentRed;

    g.fillAll(KhensuTheme::bgBase.withAlpha(0.85f));
    g.setColour(KhensuTheme::borderSubtle);
    g.drawHorizontalLine(getHeight() - 1, 0.0f, (float)getWidth());

    auto b = getLocalBounds().reduced(10, 2);
    g.setFont(KhensuTheme::labelFont(type::caption)); // UX-5: readable, sentence case

    // Segment 1: session clock
    g.setColour(KhensuTheme::textTertiary);
    g.drawText(TRANS("Session") + " " + juce::String(sessionMin / 60) + "h"
                   + juce::String(sessionMin % 60).paddedLeft('0', 2),
               b.removeFromLeft(100), juce::Justification::centredLeft, false);

    // Segment 2: time since break + nudge
    juce::String breakText =
        onBreakNow ? TRANS("On a break - good")
                   : TRANS("Last break %d min ago").replace("%d", juce::String(sinceBreakMin));
    if (!onBreakNow && sinceBreakMin >= 75)
        breakText << "  -  step away: your ears stopped telling the truth ~30 min ago";
    else if (!onBreakNow && sinceBreakMin >= 45)
        breakText << "  -  break soon - decisions drift after ~45 min";
    g.setColour(!onBreakNow && sinceBreakMin >= 45 ? fatigueCol : KhensuTheme::textTertiary);
    g.drawText(breakText, b.removeFromLeft(juce::jmax(150, b.getWidth() - 260)), juce::Justification::centredLeft,
               true);

    // Segment 3: ear-fatigue meter
    auto meter = b.removeFromLeft(150).reduced(0, 6);
    auto label = meter.removeFromLeft(76);
    g.setColour(KhensuTheme::textTertiary);
    g.drawText(TRANS("Ear fatigue"), label, juce::Justification::centredLeft, false);
    g.setColour(KhensuTheme::bgRaised);
    g.fillRoundedRectangle(meter.toFloat(), 3.0f);
    g.setColour(fatigueCol.withAlpha(0.8f));
    g.fillRoundedRectangle(meter.toFloat().withWidth(meter.getWidth() * fatigue01), 3.0f);

    // Segment 4: decision rate (renders / 10 min)
    g.setColour(KhensuTheme::textTertiary);
    juce::String rate = TRANS("%d changes in 10 min").replace("%d", juce::String(decisionsThisWindow_));
    if (decisionsPer10Min_ > 8.0f) rate << " - " << TRANS("churning?");
    g.drawText(rate, b, juce::Justification::centredRight, false);
}

} // namespace khensu
