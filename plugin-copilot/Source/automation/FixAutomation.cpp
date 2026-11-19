#include "FixAutomation.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace khensu::automation {

float Lane::valueAt(double timeSec) const noexcept
{
    if (points.empty()) return 0.0f;
    // First point strictly after timeSec (right-continuous at steps).
    const auto it = std::upper_bound(points.begin(), points.end(), timeSec,
                                     [](double t, const std::pair<double, float>& p) { return t < p.first; });
    if (it == points.begin()) return points.front().second;
    if (it == points.end()) return points.back().second;
    const auto& [t1, v1] = *it;
    const auto& [t0, v0] = *(it - 1);
    if (t1 <= t0) return v1;
    return v0 + static_cast<float>((timeSec - t0) / (t1 - t0)) * (v1 - v0);
}

double FixCurve::startSec() const noexcept
{
    double s = 0.0;
    bool any = false;
    for (const auto& lane : lanes)
        if (!lane.points.empty()) {
            s = any ? std::min(s, lane.points.front().first) : lane.points.front().first;
            any = true;
        }
    return s;
}

double FixCurve::endSec() const noexcept
{
    double e = 0.0;
    for (const auto& lane : lanes)
        if (!lane.points.empty()) e = std::max(e, lane.points.back().first);
    return e;
}

namespace {

enum class Mode { Absolute, Relative, Cut, Boost };

struct KeyMapping {
    const char* key;
    const char* paramId;
    Mode mode;
};

// Mirrors FIX_KEY_MAP in local-services/khensu_services/copilot/automation_export.py
// and the editor's auto-apply table.
constexpr KeyMapping kKeyMap[] = {
    {"comp_ratio", "comp_ratio", Mode::Absolute},
    {"comp_threshold_db", "comp_thresh_db", Mode::Absolute},
    {"comp_attack_ms", "comp_attack_ms", Mode::Absolute},
    {"comp_release_ms", "comp_release_ms", Mode::Absolute},
    {"comp_makeup_gain_db", "out_gain_db", Mode::Relative},
    {"eq_freq_hz", "eq_freq", Mode::Absolute},
    {"eq_gain_db", "eq_gain_db", Mode::Absolute},
    {"eq_q", "eq_q", Mode::Absolute},
    {"reduce_gain_db", "out_gain_db", Mode::Cut},
    {"increase_gain_db", "out_gain_db", Mode::Boost},
    {"true_peak_ceiling_dbtp", "lim_thresh_db", Mode::Absolute},
};

const char* stageSwitchFor(const std::string& paramId)
{
    if (paramId.rfind("comp_", 0) == 0 && paramId != "comp_on" && paramId != "comp_multiband") return "comp_on";
    if (paramId == "eq_freq" || paramId == "eq_gain_db" || paramId == "eq_q") return "eq_on";
    if (paramId == "lim_thresh_db") return "lim_on";
    return nullptr;
}

} // namespace

FixCurve curveFromFix(const std::map<std::string, float>& suggestedParameters, double startSec, double endSec,
                      double rampSec, const std::function<float(const std::string&)>& currentValue)
{
    FixCurve curve;
    if (!(endSec > startSec) || !std::isfinite(startSec) || !std::isfinite(endSec)) return curve;
    rampSec = std::clamp(std::isfinite(rampSec) ? rampSec : 0.0, 0.0, 10.0);

    std::map<std::string, float> targets;
    float gainDelta = 0.0f;
    bool hasGain = false;
    for (const auto& [key, raw] : suggestedParameters) {
        if (!std::isfinite(raw)) continue;
        const KeyMapping* mapping = nullptr;
        for (const auto& m : kKeyMap)
            if (key == m.key) mapping = &m;
        if (mapping == nullptr) {
            curve.unmappedKeys.push_back(key);
            continue;
        }
        switch (mapping->mode) {
        case Mode::Absolute: targets[mapping->paramId] = raw; break;
        case Mode::Relative:
            gainDelta += raw;
            hasGain = true;
            break;
        case Mode::Cut:
            gainDelta -= std::abs(raw);
            hasGain = true;
            break;
        case Mode::Boost:
            gainDelta += std::abs(raw);
            hasGain = true;
            break;
        }
    }
    const auto current = [&currentValue](const std::string& id) { return currentValue ? currentValue(id) : 0.0f; };
    if (hasGain) targets["out_gain_db"] = current("out_gain_db") + gainDelta;
    std::map<std::string, float> switches;
    for (const auto& [id, value] : targets) {
        juce::ignoreUnused(value);
        if (const char* sw = stageSwitchFor(id); sw != nullptr && targets.count(sw) == 0) switches[sw] = 1.0f;
    }
    targets.insert(switches.begin(), switches.end());

    const double laneStart = std::max(0.0, startSec - rampSec);
    for (const auto& [id, target] : targets) {
        const float base = current(id);
        Lane lane;
        lane.paramId = id;
        lane.points = {{laneStart, base}, {startSec, target}, {endSec, target}, {endSec + rampSec, base}};
        curve.lanes.push_back(std::move(lane));
    }
    return curve;
}

void FixAutomationWriter::arm(FixCurve curve, IGestureSink& sink)
{
    endAll(sink);
    curve_ = std::move(curve);
    open_.assign(curve_.lanes.size(), false);
    lastSent_.assign(curve_.lanes.size(), std::numeric_limits<float>::quiet_NaN());
    progress_ = 0.0;
    valuesWritten_ = 0;
    state_ = curve_.empty() ? State::Idle : State::Armed;
}

void FixAutomationWriter::cancel(IGestureSink& sink)
{
    endAll(sink);
    state_ = State::Idle;
}

void FixAutomationWriter::endAll(IGestureSink& sink)
{
    for (size_t i = 0; i < open_.size() && i < curve_.lanes.size(); ++i)
        if (open_[i]) {
            sink.endGesture(curve_.lanes[i].paramId);
            open_[i] = false;
        }
}

void FixAutomationWriter::writeAt(double timeSec, IGestureSink& sink)
{
    for (size_t i = 0; i < curve_.lanes.size(); ++i) {
        const auto& lane = curve_.lanes[i];
        if (!open_[i]) open_[i] = sink.beginGesture(lane.paramId);
        const float v = lane.valueAt(timeSec);
        if (std::isnan(lastSent_[i]) || std::abs(v - lastSent_[i]) > kEpsilon) {
            sink.setValue(lane.paramId, v);
            lastSent_[i] = v;
            ++valuesWritten_;
        }
    }
}

void FixAutomationWriter::tick(double hostTimeSec, bool isPlaying, IGestureSink& sink)
{
    if (state_ == State::Idle || state_ == State::Finished || curve_.empty()) return;
    const double start = curve_.startSec();
    const double end = curve_.endSec();

    if (state_ == State::Armed) {
        // Only start when the playhead passes INTO the span while playing, so
        // the host sees the gesture begin at the first automated point.
        if (isPlaying && hostTimeSec >= start && hostTimeSec < end) {
            state_ = State::Writing;
            writeAt(hostTimeSec, sink);
            progress_ = (hostTimeSec - start) / std::max(1e-9, end - start);
        }
        return;
    }

    // Writing.
    if (!isPlaying || hostTimeSec < start - 0.25) {
        // Stopped or jumped back before the span: close the gestures and
        // re-arm, so the next pass writes the curve again from its start.
        endAll(sink);
        lastSent_.assign(curve_.lanes.size(), std::numeric_limits<float>::quiet_NaN());
        state_ = State::Armed;
        return;
    }
    if (hostTimeSec >= end) {
        writeAt(end, sink); // land exactly on the final (restored) values
        endAll(sink);
        progress_ = 1.0;
        state_ = State::Finished;
        return;
    }
    writeAt(hostTimeSec, sink);
    progress_ = (hostTimeSec - start) / std::max(1e-9, end - start);
}

} // namespace khensu::automation
