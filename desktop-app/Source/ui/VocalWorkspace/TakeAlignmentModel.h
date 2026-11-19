// desktop-app/Source/ui/VocalWorkspace/TakeAlignmentModel.h
// Request/response model for doubles & ad-lib timing alignment
// (POST /analyze/align_takes). Pure data - unit-tested directly.
#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

struct TakeAlignmentRequest {
    juce::String referencePath;
    juce::StringArray takePaths;
    bool quantize = false; ///< false = align to the lead, true = snap to grid
    double bpm = 0.0;      ///< 0 = let the service estimate it
    int gridDivision = 16; ///< 4, 8, 16, 32
    double strength = 1.0; ///< 0..1
    double maxShiftMs = 80.0;
    bool render = true;
    /// One fresh output file per take (uniqueRenderFile), so a re-run never
    /// overwrites a render the history / transport still references (UIX-11).
    /// Sent only when it matches takePaths one-to-one; empty = server default.
    juce::StringArray outputPaths;

    bool isValid() const
    {
        return referencePath.isNotEmpty() && !takePaths.isEmpty() && takePaths.size() <= 8 &&
               (gridDivision == 4 || gridDivision == 8 || gridDivision == 16 || gridDivision == 32);
    }

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty(api::fields::AlignTakesRequest::reference_path, referencePath);
        juce::Array<juce::var> takes;
        for (const auto& t : takePaths)
            takes.add(t);
        o->setProperty(api::fields::AlignTakesRequest::take_paths, takes);
        o->setProperty(api::fields::AlignTakesRequest::mode, quantize ? "quantize" : "align");
        o->setProperty(api::fields::AlignTakesRequest::bpm, bpm > 0.0 ? juce::var(bpm) : juce::var());
        o->setProperty(api::fields::AlignTakesRequest::grid_division, gridDivision);
        o->setProperty(api::fields::AlignTakesRequest::strength, juce::jlimit(0.0, 1.0, strength));
        o->setProperty(api::fields::AlignTakesRequest::max_shift_ms, maxShiftMs);
        o->setProperty(api::fields::AlignTakesRequest::render, render);
        if (render && outputPaths.size() == takePaths.size() && !outputPaths.isEmpty()) {
            juce::Array<juce::var> outs;
            for (const auto& p : outputPaths)
                outs.add(p);
            o->setProperty(api::fields::AlignTakesRequest::output_paths, outs);
        }
        return juce::var(o);
    }
};

struct TakeAlignmentResult {
    struct Segment {
        double startSec = 0.0, endSec = 0.0, shiftMs = 0.0;
    };
    struct Take {
        juce::String path;
        std::vector<Segment> segments;
        double meanAbsBeforeMs = 0.0, meanAbsAfterMs = 0.0;
        juce::String renderedPath;

        double improvementMs() const noexcept { return meanAbsBeforeMs - meanAbsAfterMs; }
    };

    juce::String mode;
    double bpm = 0.0;
    std::vector<Take> takes;

    static TakeAlignmentResult fromVar(const juce::var& v)
    {
        TakeAlignmentResult r;
        r.mode = v[api::fields::AlignTakesResponse::mode].toString();
        r.bpm = v[api::fields::AlignTakesResponse::bpm].isVoid() ? 0.0 : static_cast<double>(v[api::fields::AlignTakesResponse::bpm]);
        if (auto* arr = v[api::fields::AlignTakesResponse::takes].getArray()) {
            for (const auto& tv : *arr) {
                Take t;
                t.path = tv[api::fields::AlignedTake::path].toString();
                t.meanAbsBeforeMs = static_cast<double>(tv[api::fields::AlignedTake::mean_abs_offset_ms_before]);
                t.meanAbsAfterMs = static_cast<double>(tv[api::fields::AlignedTake::mean_abs_offset_ms_after]);
                t.renderedPath = tv[api::fields::AlignedTake::rendered_path].isVoid() ? juce::String() : tv[api::fields::AlignedTake::rendered_path].toString();
                if (auto* segs = tv[api::fields::AlignedTake::segments].getArray())
                    for (const auto& sv : *segs)
                        t.segments.push_back({static_cast<double>(sv[api::fields::AlignSegment::start_sec]), static_cast<double>(sv[api::fields::AlignSegment::end_sec]),
                                              static_cast<double>(sv[api::fields::AlignSegment::shift_ms])});
                r.takes.push_back(std::move(t));
            }
        }
        return r;
    }

    double maxAbsShiftMs() const noexcept
    {
        double m = 0.0;
        for (const auto& t : takes)
            for (const auto& s : t.segments)
                m = std::max(m, std::abs(s.shiftMs));
        return m;
    }

    double durationSec() const noexcept
    {
        double d = 0.0;
        for (const auto& t : takes)
            if (!t.segments.empty()) d = std::max(d, t.segments.back().endSec);
        return d;
    }

    juce::String summary() const
    {
        if (takes.empty()) return "No takes aligned.";
        juce::String s;
        for (const auto& t : takes) {
            if (s.isNotEmpty()) s << "; ";
            s << juce::File(t.path).getFileNameWithoutExtension() << ": " << juce::String(t.meanAbsBeforeMs, 1)
              << " -> " << juce::String(t.meanAbsAfterMs, 1) << " ms";
        }
        return s;
    }
};

} // namespace khensu
