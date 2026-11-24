// desktop-app/Source/ui/shared/TimelineMarkers.h
// VIZ-2c: where in the song each finding happens. A finding with its own time
// range (hooks, section flow, ...) is placed there; the findings that are
// built from moments the timeline measures are placed on those moments:
//   true_peak_over     - each true-peak event over the threshold
//   clipping_detected  - each run of samples at full scale
//   harshness / sibilance findings - the section that measures harshest
// A finding that happens throughout gets no marker (it is on the hero).
// THREAD_CONTEXT(UIThread) - plain functions, tested in TimelineMarkersTest.
#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <string>
#include <vector>
#include <khensu/analysis/SectionAnalysis.h>
#include <khensu/analysis/TrackTimeline.h>
#include "../../services/IIssues.h"
#include "SongTimelineView.h"

namespace khensu::timeline_markers {

inline constexpr size_t kMaxEventsPerFinding = 24;

inline bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

inline juce::String titleOf(const IIssues::Issue& i)
{
    const auto& text = i.rec.what.empty() ? i.rec.suggested_action : i.rec.what;
    return juce::String(juce::CharPointer_UTF8(text.c_str())).upToFirstOccurrenceOf("\n", false, false);
}

inline std::vector<SongTimelineView::Marker> build(const std::vector<IIssues::Issue>& issues, const TrackTimeline* tl,
                                                   const std::vector<SectionData>& sections)
{
    std::vector<SongTimelineView::Marker> out;
    for (const auto& i : issues) {
        if (i.settled() || i.rec.type == RecommendationType::Strength) continue;
        const std::string key = i.recId.empty() ? i.code : i.recId;
        SongTimelineView::Marker base;
        base.key = key;
        base.title = titleOf(i);
        base.severity = i.rec.severity;
        if (i.rec.freq_range && i.rec.freq_range->is_valid()) {
            base.lowHz = i.rec.freq_range->low_hz;
            base.highHz = i.rec.freq_range->high_hz;
        }
        if (i.rec.time_range && i.rec.time_range->end_sec >= i.rec.time_range->start_sec) {
            auto m = base;
            m.startSec = i.rec.time_range->start_sec;
            m.endSec = i.rec.time_range->end_sec;
            out.push_back(m);
            continue;
        }
        if (startsWith(i.code, "true_peak_over") && tl != nullptr) {
            for (size_t k = 0; k < tl->truePeakEvents.size() && k < kMaxEventsPerFinding; ++k) {
                auto m = base;
                m.startSec = tl->truePeakEvents[k].startSec;
                m.endSec = tl->truePeakEvents[k].endSec;
                m.title = juce::String(tl->truePeakEvents[k].maxDbtp, 1) + " dBTP - " + base.title;
                out.push_back(m);
            }
            continue;
        }
        if (startsWith(i.code, "clipping_detected") && tl != nullptr) {
            size_t added = 0;
            for (size_t h = 0; h < tl->peakDbfs.size() && added < kMaxEventsPerFinding; ++h) {
                if (tl->peakDbfs[h] < -0.01f) continue;
                size_t end = h;
                while (end + 1 < tl->peakDbfs.size() && tl->peakDbfs[end + 1] >= -0.01f)
                    ++end;
                auto m = base;
                m.startSec = static_cast<double>(h) * tl->hopSec;
                m.endSec = static_cast<double>(end + 1) * tl->hopSec;
                out.push_back(m);
                ++added;
                h = end;
            }
            continue;
        }
        const bool harsh = startsWith(i.code, "harshness");
        const bool sib = startsWith(i.code, "sibilance") || i.code.find("sibilan") != std::string::npos;
        if ((harsh || sib) && !sections.empty()) {
            const SectionData* worst = nullptr;
            for (const auto& s : sections) {
                const float v = harsh ? s.harshness_score : s.sibilance_score;
                if (v < 0.0f) continue;
                if (worst == nullptr || v > (harsh ? worst->harshness_score : worst->sibilance_score)) worst = &s;
            }
            if (worst != nullptr) {
                auto m = base;
                m.startSec = worst->start_sec;
                m.endSec = worst->end_sec;
                if (m.highHz <= 0.0f) {
                    m.lowHz = harsh ? 2500.0f : 5000.0f;
                    m.highHz = harsh ? 5000.0f : 10000.0f;
                }
                m.title = base.title + " (" + TRANS("worst in") + " "
                          + juce::String(juce::CharPointer_UTF8(worst->label.c_str())) + ")";
                out.push_back(m);
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.startSec < b.startSec; });
    return out;
}

} // namespace khensu::timeline_markers
