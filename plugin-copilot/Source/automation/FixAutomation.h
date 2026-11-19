#pragma once

#include <JuceHeader.h>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace khensu::automation {

/// A parameter curve in host time, in PLAIN parameter units.
struct Lane {
    std::string paramId;
    std::vector<std::pair<double, float>> points; ///< (time sec, value), sorted by time

    /// Linear between points, held before the first and after the last;
    /// right-continuous at a step (two points at one time).
    float valueAt(double timeSec) const noexcept;
};

/// A fix as automation: one lane per Copilot parameter it moves.
struct FixCurve {
    std::vector<Lane> lanes;
    std::vector<std::string> unmappedKeys; ///< suggested parameters the Copilot has no control for
    std::string label;

    bool empty() const noexcept { return lanes.empty(); }
    double startSec() const noexcept;
    double endSec() const noexcept;
};

/// Maps a recommendation's suggested_parameters onto the Copilot's own
/// parameters (same table as the editor's auto-apply and the service's
/// /automation/export) and ramps them: the current value until
/// startSec - rampSec, the target over [startSec, endSec], back to the
/// current value by endSec + rampSec. Gain keys are relative moves of
/// out_gain_db; the stage switch of every touched stage is turned on.
FixCurve curveFromFix(const std::map<std::string, float>& suggestedParameters, double startSec, double endSec,
                      double rampSec, const std::function<float(const std::string&)>& currentValue);

/// Where the writer sends its gestures. The processor implements it with
/// beginChangeGesture / setValueNotifyingHost / endChangeGesture.
class IGestureSink {
  public:
    virtual ~IGestureSink() = default;
    virtual bool beginGesture(const std::string& paramId) = 0;
    virtual void setValue(const std::string& paramId, float plainValue) = 0;
    virtual void endGesture(const std::string& paramId) = 0;
};

/// Plays a FixCurve onto the plugin's parameters while the host plays
/// through its time span, inside change gestures, so a host whose automation
/// is in write / touch / latch mode records it (CE-DAW-4). Pure logic: the
/// caller drives tick() from the message thread with the host position.
class FixAutomationWriter {
  public:
    enum class State { Idle, Armed, Writing, Finished };

    /// Arms a new curve (ends any gesture of the previous one first).
    void arm(FixCurve curve, IGestureSink& sink);
    /// Ends any open gesture and returns to Idle.
    void cancel(IGestureSink& sink);
    /// THREAD_CONTEXT(UIThread). One step at host time `hostTimeSec`.
    void tick(double hostTimeSec, bool isPlaying, IGestureSink& sink);

    State state() const noexcept { return state_; }
    const FixCurve& curve() const noexcept { return curve_; }
    /// 0..1 through the curve's span during Writing.
    double progress() const noexcept { return progress_; }
    /// Value changes sent to the sink since arm().
    int valuesWritten() const noexcept { return valuesWritten_; }

    /// Values closer than this are not re-sent.
    static constexpr float kEpsilon = 1.0e-4f;

  private:
    void endAll(IGestureSink& sink);
    void writeAt(double timeSec, IGestureSink& sink);

    FixCurve curve_;
    State state_ = State::Idle;
    std::vector<bool> open_;
    std::vector<float> lastSent_;
    double progress_ = 0.0;
    int valuesWritten_ = 0;
};

} // namespace khensu::automation
