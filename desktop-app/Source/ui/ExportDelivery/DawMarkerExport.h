// desktop-app/Source/ui/ExportDelivery/DawMarkerExport.h
// STU-4: the session's sections and timestamped findings as DAW markers.
// The findings are the timeline's (TimelineMarkers.h: a finding's own time
// range, each true-peak event, each clipping run, the harshest section), so
// a marker sits where the Analysis timeline shows it. The writers are
// shared-core's (khensu/session/DawMarkers.h). THREAD_CONTEXT(Any) - plain
// functions; ReportPanel collects on the message thread and writes on a worker.
#pragma once
#include <JuceHeader.h>
#include <vector>
#include <khensu/analysis/SectionAnalysis.h>
#include <khensu/session/DawMarkers.h>
#include "../shared/SongTimelineView.h"

namespace khensu::daw_marker_export {

enum class Format { Csv, ReaperCsv, Midi };

inline const char* extension(Format f) noexcept
{
    switch (f) {
    case Format::Csv: return ".csv";
    case Format::ReaperCsv: return ".csv";
    case Format::Midi: return ".mid";
    }
    return ".csv";
}

/// The file name suffix that tells the two CSV files apart.
inline const char* suffix(Format f) noexcept
{
    switch (f) {
    case Format::Csv: return "_markers";
    case Format::ReaperCsv: return "_reaper_regions";
    case Format::Midi: return "_markers";
    }
    return "_markers";
}

inline std::vector<daw_markers::Marker> collect(const std::vector<SectionData>& sections,
                                                const std::vector<SongTimelineView::Marker>& findings)
{
    std::vector<daw_markers::Marker> out;
    for (const auto& s : sections) {
        if (s.end_sec <= s.start_sec) continue;
        daw_markers::Marker m;
        m.name = s.label.empty() ? (s.role.empty() ? std::string("Section") : s.role) : s.label;
        m.startSec = s.start_sec;
        m.endSec = s.end_sec;
        m.kind = daw_markers::Kind::Section;
        out.push_back(std::move(m));
    }
    for (const auto& f : findings) {
        daw_markers::Marker m;
        m.name = f.title.trim().toStdString();
        m.startSec = f.startSec;
        m.endSec = std::max(f.endSec, f.startSec);
        m.kind = daw_markers::Kind::Finding;
        out.push_back(std::move(m));
    }
    return out;
}

/// Writes `markers` to `file` in `format`; false when the file cannot be written.
inline bool write(const juce::File& file, Format format, const std::vector<daw_markers::Marker>& markers, double bpm)
{
    if (format == Format::Midi) {
        const auto bytes = daw_markers::toMidiFile(markers, bpm);
        return file.replaceWithData(bytes.data(), bytes.size());
    }
    const auto text = format == Format::Csv ? daw_markers::toCsv(markers) : daw_markers::toReaperCsv(markers);
    return file.replaceWithData(text.data(), text.size());
}

} // namespace khensu::daw_marker_export
