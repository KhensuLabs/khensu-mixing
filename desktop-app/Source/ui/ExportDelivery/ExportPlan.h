// desktop-app/Source/ui/ExportDelivery/ExportPlan.h
#pragma once
#include <JuceHeader.h>
#include <cmath>
#include <optional>
#include <vector>
#include "../../export/AudioExporter.h"
#include <khensu/ipc/HttpServiceBridge.h>

/// Pure export-planning helpers for ExportScreen / FormatPanel - no UI, no
/// I/O beyond juce::File path arithmetic, so they are unit-tested directly
/// (tests/ui/ExportPlanTest.cpp).
namespace khensu::export_plan {

/// The file extension (with the dot) a format writes. The output path follows
/// the format, so picking FLAC after choosing "export.wav" can't write FLAC
/// bytes into a .wav (UIX-03).
inline juce::String extensionFor(AudioExportOptions::Format format)
{
    switch (format) {
    case AudioExportOptions::Format::MP3: return ".mp3";
    case AudioExportOptions::Format::AAC: return ".m4a";
    case AudioExportOptions::Format::AIFF: return ".aiff";
    case AudioExportOptions::Format::FLAC: return ".flac";
    case AudioExportOptions::Format::WAV: break;
    }
    return ".wav";
}

/// `path` with its extension replaced to match `format` (empty stays empty).
inline juce::String withFormatExtension(const juce::String& path, AudioExportOptions::Format format)
{
    if (path.isEmpty()) return {};
    return juce::File(path).withFileExtension(extensionFor(format)).getFullPathName();
}

/// A legal default file name for a session's export: the session name made
/// legal on every OS (UIX-04 / UIX-21: "Mix: v2?" broke Windows), or
/// "export" when nothing usable is left.
inline juce::String defaultFileName(const juce::String& sessionName, AudioExportOptions::Format format)
{
    juce::String base = juce::File::createLegalFileName(sessionName.trim()).trim();
    while (base.endsWithChar('.'))
        base = base.dropLastCharacters(1);
    if (base.isEmpty()) base = "export";
    return base + extensionFor(format);
}

/// Why an export must not write to `output`, or empty when it may. Never the
/// session's original, the file being exported, or any file the render
/// history references - those would destroy the session's own audio (UIX-04).
inline juce::String rejectOutputPath(const juce::String& output, const juce::String& sessionSource,
                                     const juce::String& exportSource, const std::vector<juce::String>& renderPaths)
{
    if (output.isEmpty()) return TRANS("Choose an output file path in the Format panel.");
    const juce::File out(output);
    if (sessionSource.isNotEmpty() && out == juce::File(sessionSource))
        return TRANS("That is the session's original file - choose a different export path.");
    if (exportSource.isNotEmpty() && out == juce::File(exportSource))
        return TRANS("That is the file being exported - choose a different export path.");
    for (const auto& r : renderPaths)
        if (r.isNotEmpty() && out == juce::File(r))
            return TRANS("That file is in the session's render history - choose a different export path.");
    return {};
}

/// The gain (dB) the export applies. "As mastered" applies none (UIX-02).
inline float plannedGainDb(bool normalize, float measuredLufs, float targetLufs, float truePeakDbtp, float ceilingDbtp)
{
    if (!normalize) return 0.0f;
    const float lin = AudioExporter::computeGain(measuredLufs, targetLufs, truePeakDbtp, ceilingDbtp);
    return 20.0f * std::log10(juce::jmax(1.0e-6f, lin));
}

/// Re-grades a compliance result measured on the SOURCE as it will read
/// after the export gain (UIX-05): integrated loudness, true peak and the
/// short-term max all move by `gainDb`; loudness range and channel count
/// don't. Mirrors delivery_targets.evaluate's rules: the integrated window
/// fails only when the target has a tolerance (else it warns, with a 0.5 LU
/// window), true peak over the ceiling fails, the short-term cap warns.
inline ipc::HttpServiceBridge::DeliveryTargetResult
predictAfterGain(ipc::HttpServiceBridge::DeliveryTargetResult measured, double gainDb,
                 std::optional<double> toleranceLu)
{
    if (std::abs(gainDb) < 1.0e-6) return measured;
    for (auto& c : measured.checks) {
        if (!c.measured.has_value()) continue;
        if (c.id == "integrated") {
            c.measured = *c.measured + gainDb;
            if (c.target.has_value()) {
                c.delta = *c.measured - *c.target;
                const bool within = std::abs(*c.delta) <= toleranceLu.value_or(0.5);
                c.status = within ? "pass" : (toleranceLu.has_value() ? "fail" : "warn");
                c.note = juce::String(*c.measured, 1).toStdString() + " LUFS after the export gain, against "
                         + juce::String(*c.target, 1).toStdString() + " LUFS.";
                c.fix = within ? std::string() : std::string("Adjust the export target or the master's level.");
            }
        } else if (c.id == "true_peak") {
            c.measured = *c.measured + gainDb;
            if (c.target.has_value()) {
                c.delta = *c.measured - *c.target;
                const bool over = *c.measured > *c.target;
                c.status = over ? "fail" : "pass";
                c.note = "True peak " + juce::String(*c.measured, 1).toStdString() + " dBTP after the export gain"
                         + (over ? std::string(" - over the ceiling.") : std::string(" - within the ceiling."));
                c.fix = over ? std::string("True-peak limit the master before exporting.") : std::string();
            }
        } else if (c.id == "short_term_max") {
            c.measured = *c.measured + gainDb;
            if (c.target.has_value()) {
                c.delta = *c.measured - *c.target;
                c.status = *c.measured > *c.target ? "warn" : "pass";
            }
        }
    }
    bool anyFail = false, anyWarn = false;
    for (const auto& c : measured.checks) {
        anyFail = anyFail || c.status == "fail";
        anyWarn = anyWarn || c.status == "warn";
    }
    measured.overall = anyFail ? "fail" : anyWarn ? "warn" : "pass";
    return measured;
}

} // namespace khensu::export_plan
