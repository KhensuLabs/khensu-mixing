// desktop-app/Source/ui/shared/ValueAnimator.h
// UI-7: short eased transitions that explain a change - a score moving from
// its old value to its new one, a finding's bar shrinking after a fix, a card
// fading in when it appears. Nothing here loops or decorates: an animation
// runs once, 150-250 ms, when a value the user is reading changes.
//
//   ValueAnimator score_{[this] { repaint(); }};   // declare after what it touches
//   score_.setTarget(newScore);                      // first call jumps, later ones ease
//   g.drawText(String(roundToInt(score_.value())), ...);
//
// motion::fade(component, visible) fades a component in or out instead of
// popping it.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <functional>

namespace khensu {

namespace motion {

/// The app's one transition length (UI-7: 150-250 ms).
inline constexpr int kDurationMs = 200;

/// Ease-out cubic: fast start, gentle landing - a value "arrives".
inline float easeOutCubic(float t) noexcept
{
    t = juce::jlimit(0.0f, 1.0f, t);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

/// A value moving from `from` to `to` between `startMs` and startMs + durationMs.
/// Pure (the caller supplies the clock), so it is testable without a timer.
struct Tween {
    float from = 0.0f;
    float to = 0.0f;
    double startMs = 0.0;
    int durationMs = kDurationMs;

    float valueAt(double nowMs) const noexcept
    {
        if (durationMs <= 0) return to;
        const auto t = static_cast<float>((nowMs - startMs) / static_cast<double>(durationMs));
        return from + (to - from) * easeOutCubic(t);
    }
    bool finishedAt(double nowMs) const noexcept { return nowMs - startMs >= static_cast<double>(durationMs); }
};

/// Fades `c` in (visible) or out (hidden) over the app's transition length.
/// A component that already has the asked-for visibility is left alone.
inline void fade(juce::Component& c, bool visible, int durationMs = kDurationMs)
{
    if (c.isVisible() == visible) return;
    auto& animator = juce::Desktop::getInstance().getAnimator();
    if (visible)
        animator.fadeIn(&c, durationMs);
    else
        animator.fadeOut(&c, durationMs);
}

} // namespace motion

/// A float that eases to each new target and calls `onFrame` (usually
/// repaint) on every step. The first target is taken at once - there is no
/// "old value" to move from - and so is any target while `animate` is false.
class ValueAnimator final : private juce::Timer {
  public:
    explicit ValueAnimator(std::function<void()> onFrame, int durationMs = motion::kDurationMs)
        : onFrame_(std::move(onFrame)), durationMs_(durationMs)
    {
    }
    ~ValueAnimator() override { stopTimer(); }

    void setTarget(float target, bool animate = true)
    {
        if (!hasValue_ || !animate || durationMs_ <= 0) {
            jumpTo(target);
            return;
        }
        if (juce::approximatelyEqual(target, tween_.to) && !isTimerRunning()) return;
        tween_ = {value(), target, now(), durationMs_};
        current_ = tween_.from;
        startTimerHz(120); // UX2-7: a 120 Hz display gets every frame; the tween is time-based
    }

    void jumpTo(float target)
    {
        stopTimer();
        hasValue_ = true;
        tween_ = {target, target, now(), 0};
        current_ = target;
        if (onFrame_) onFrame_();
    }

    /// Forgets the value: the next setTarget() jumps.
    void reset() noexcept
    {
        stopTimer();
        hasValue_ = false;
    }

    float value() const noexcept { return current_; }
    float target() const noexcept { return tween_.to; }
    bool hasValue() const noexcept { return hasValue_; }
    bool isAnimating() const noexcept { return isTimerRunning(); }

  private:
    static double now() { return juce::Time::getMillisecondCounterHiRes(); }

    void timerCallback() override
    {
        const double t = now();
        current_ = tween_.valueAt(t);
        if (tween_.finishedAt(t)) {
            current_ = tween_.to;
            stopTimer();
        }
        if (onFrame_) onFrame_();
    }

    std::function<void()> onFrame_;
    int durationMs_;
    motion::Tween tween_;
    float current_ = 0.0f;
    bool hasValue_ = false;

    JUCE_DECLARE_NON_COPYABLE(ValueAnimator)
};

} // namespace khensu
