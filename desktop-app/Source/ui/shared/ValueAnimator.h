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
//
// STU-11: ValueAnimator::Curve::Spring follows a critically damped spring
// instead of the fixed-length ease. A new target set mid-flight keeps the
// value's velocity, so a morph that is retargeted bends smoothly instead of
// restarting from rest - better for values that update often (a camera, a
// spectrum morph). Critically damped means it never overshoots from rest.
//
// VIS-7: with reduced motion on (ui::motion::reduced(), ReducedMotion.h) a
// setTarget() jumps to its value and a fade shows or hides at once.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "ReducedMotion.h"
#include <algorithm>
#include <cmath>
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

/// STU-11: a critically damped spring from `from` (moving at `velocity`
/// units/s) towards `to`, started at startMs:
///   x(t) = to + (c1 + c2 t) e^(-w t),  c1 = from - to,  c2 = velocity + w c1.
/// From rest it approaches `to` monotonically (no overshoot). `omega` is the
/// natural frequency in rad/s; springOmegaFor() picks it so the spring is
/// within 0.1 % of its travel after a given time. Pure, like Tween.
struct Spring {
    float from = 0.0f;
    float to = 0.0f;
    float velocity = 0.0f; ///< units per second at startMs
    double startMs = 0.0;
    float omega = 46.0f;

    float valueAt(double nowMs) const noexcept
    {
        const double t = std::max(0.0, (nowMs - startMs) / 1000.0);
        const double c1 = static_cast<double>(from) - to;
        const double c2 = static_cast<double>(velocity) + omega * c1;
        return static_cast<float>(to + (c1 + c2 * t) * std::exp(-static_cast<double>(omega) * t));
    }
    float velocityAt(double nowMs) const noexcept
    {
        const double t = std::max(0.0, (nowMs - startMs) / 1000.0);
        const double c1 = static_cast<double>(from) - to;
        const double c2 = static_cast<double>(velocity) + omega * c1;
        return static_cast<float>((c2 - omega * (c1 + c2 * t)) * std::exp(-static_cast<double>(omega) * t));
    }
    /// Close enough to stop: within 0.1 % of the travel (at least 1e-4) and
    /// moving less than that per 60 Hz frame - or 4 time constants past the
    /// settle time, so a timer always stops.
    bool settledAt(double nowMs) const noexcept
    {
        const float tol = std::max(1.0e-4f, 1.0e-3f * std::abs(from - to));
        const double t = (nowMs - startMs) / 1000.0;
        if (t * omega > 40.0) return true;
        return std::abs(valueAt(nowMs) - to) <= tol && std::abs(velocityAt(nowMs)) / 60.0f <= tol;
    }
};

/// The natural frequency at which a critically damped spring released from
/// rest has covered 99.9 % of its travel after `settleMs` ((1 + wt) e^-wt =
/// 0.001 at wt ~= 9.23).
inline float springOmegaFor(int settleMs) noexcept
{
    return settleMs > 0 ? 9.23f * 1000.0f / static_cast<float>(settleMs) : 1.0e6f;
}

/// Fades `c` in (visible) or out (hidden) over the app's transition length.
/// A component that already has the asked-for visibility is left alone.
inline void fade(juce::Component& c, bool visible, int durationMs = kDurationMs)
{
    if (c.isVisible() == visible) return;
    if (ui::motion::reduced() || durationMs <= 0) {
        c.setAlpha(1.0f);
        c.setVisible(visible);
        return;
    }
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
/// Curve::Ease (default) is the 200 ms ease-out; Curve::Spring a critically
/// damped spring that settles in about `durationMs` and keeps its velocity
/// when retargeted (STU-11).
class ValueAnimator final : private juce::Timer {
  public:
    enum class Curve { Ease, Spring };

    explicit ValueAnimator(std::function<void()> onFrame, int durationMs = motion::kDurationMs,
                           Curve curve = Curve::Ease)
        : onFrame_(std::move(onFrame)), durationMs_(durationMs), curve_(curve)
    {
    }
    ~ValueAnimator() override { stopTimer(); }

    void setTarget(float target, bool animate = true)
    {
        if (!hasValue_ || !animate || durationMs_ <= 0 || ui::motion::reduced()) {
            jumpTo(target);
            return;
        }
        if (juce::approximatelyEqual(target, tween_.to) && !isTimerRunning()) return;
        const double t = now();
        if (curve_ == Curve::Spring) {
            const float v = isTimerRunning() ? spring_.velocityAt(t) : 0.0f;
            spring_ = {value(), target, v, t, motion::springOmegaFor(durationMs_)};
        }
        tween_ = {value(), target, t, durationMs_};
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
    Curve curve() const noexcept { return curve_; }

  private:
    static double now() { return juce::Time::getMillisecondCounterHiRes(); }

    void timerCallback() override
    {
        const double t = now();
        if (ui::motion::reduced()) { // turned on mid-flight: land now
            jumpTo(tween_.to);
            return;
        }
        const bool spring = curve_ == Curve::Spring;
        current_ = spring ? spring_.valueAt(t) : tween_.valueAt(t);
        if (spring ? spring_.settledAt(t) : tween_.finishedAt(t)) {
            current_ = tween_.to;
            stopTimer();
        }
        if (onFrame_) onFrame_();
    }

    std::function<void()> onFrame_;
    int durationMs_;
    Curve curve_;
    motion::Tween tween_;   ///< from / to for both curves; the timing for Ease
    motion::Spring spring_; ///< the timing for Spring
    float current_ = 0.0f;
    bool hasValue_ = false;

    JUCE_DECLARE_NON_COPYABLE(ValueAnimator)
};

} // namespace khensu
