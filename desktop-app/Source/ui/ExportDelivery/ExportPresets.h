// desktop-app/Source/ui/ExportDelivery/ExportPresets.h
// AUTO-2: export presets - the built-in delivery presets and the user's own
// (format, bit depth, sample rate, MP3 bitrate, loudness target and a name
// suffix), persisted as one JSON array in the preferences store - and the
// naming of a multi-format export: one Export press writes the chosen file
// plus one file per extra preset, each named after the original with that
// preset's suffix. 16-bit exports are always TPDF-dithered by AudioExporter
// (never truncated), so a preset's dither follows its bit depth.
// Pure: no UI, no I/O beyond path arithmetic (tests/ui/ExportPresetsTest.cpp).
// THREAD_CONTEXT(AnyThread)
#pragma once
#include <JuceHeader.h>
#include "../../export/AudioExporter.h"
#include "ExportPlan.h"
#include <algorithm>
#include <set>
#include <vector>

namespace khensu::export_presets {

using Format = AudioExportOptions::Format;
using BitDepth = AudioExportOptions::BitDepth;
using SampleRate = AudioExportOptions::SampleRate;
using Mp3Bitrate = AudioExportOptions::Mp3Bitrate;

/// The loudness keys a preset can carry: "" = as mastered (no gain),
/// "custom:<lufs>" = a custom LUFS target, anything else = an export-target id
/// from GET /analyze/export-targets.
inline juce::String customLoudnessKey(float lufs) { return "custom:" + juce::String(lufs, 1); }
inline bool isCustomLoudness(const juce::String& key) { return key.startsWith("custom:"); }
inline float customLoudnessLufs(const juce::String& key)
{
    return juce::jlimit(-30.0f, -6.0f, key.fromFirstOccurrenceOf("custom:", false, false).getFloatValue());
}

struct Preset {
    juce::String name;
    Format format = Format::WAV;
    BitDepth bitDepth = BitDepth::Bits24;
    SampleRate sampleRate = SampleRate::Source;
    Mp3Bitrate mp3Bitrate = Mp3Bitrate::kbps320;
    juce::String loudness; ///< see the keys above
    juce::String suffix;   ///< appended to the original's name, e.g. "_cd"
    int builtInId = 0;     ///< FormatPanel's preset-box id for a built-in, 0 for the user's
    bool builtIn() const noexcept { return builtInId != 0; }
};

inline constexpr const char* kPreferenceKey = "export_user_presets";

/// The fixed delivery presets (FormatPanel's box ids 2..6).
inline std::vector<Preset> builtIns()
{
    return {
        {TRANS("Streaming Master (WAV 24/44.1)"),
         Format::WAV,
         BitDepth::Bits24,
         SampleRate::SR44100,
         Mp3Bitrate::kbps320,
         {},
         "_streaming",
         2},
        {TRANS("CD Master (WAV 16/44.1 + dither)"),
         Format::WAV,
         BitDepth::Bits16,
         SampleRate::SR44100,
         Mp3Bitrate::kbps320,
         {},
         "_cd",
         3},
        {TRANS("Video / Broadcast (WAV 24/48)"),
         Format::WAV,
         BitDepth::Bits24,
         SampleRate::SR48000,
         Mp3Bitrate::kbps320,
         {},
         "_video",
         4},
        {TRANS("Hi-Res Archive (FLAC 24/96)"),
         Format::FLAC,
         BitDepth::Bits24,
         SampleRate::SR96000,
         Mp3Bitrate::kbps320,
         {},
         "_archive",
         5},
        {TRANS("Client Preview (MP3 320)"),
         Format::MP3,
         BitDepth::Bits24,
         SampleRate::Source,
         Mp3Bitrate::kbps320,
         {},
         "_preview",
         6},
    };
}

inline juce::String formatId(Format f)
{
    switch (f) {
    case Format::AIFF:
        return "aiff";
    case Format::FLAC:
        return "flac";
    case Format::MP3:
        return "mp3";
    case Format::AAC:
        return "aac";
    case Format::WAV:
        break;
    }
    return "wav";
}

inline Format formatFromId(const juce::String& id)
{
    if (id == "aiff") return Format::AIFF;
    if (id == "flac") return Format::FLAC;
    if (id == "mp3") return Format::MP3;
    if (id == "aac") return Format::AAC;
    return Format::WAV;
}

/// A suffix that is safe inside a file name: letters, digits, '-' and '_'
/// only, at most 40 characters, starting with '_' (empty in, empty out).
inline juce::String sanitizeSuffix(const juce::String& raw)
{
    juce::String out;
    for (auto c : raw.trim())
        if (juce::CharacterFunctions::isLetterOrDigit(c) || c == '-' || c == '_')
            out += juce::String::charToString(c);
        else if (c == ' ')
            out += "_";
    out = out.substring(0, 40);
    if (out.isNotEmpty() && !out.startsWithChar('_')) out = "_" + out;
    return out;
}

/// A suffix from a preset name ("Vinyl cut" -> "_vinyl_cut").
inline juce::String suffixForName(const juce::String& name)
{
    auto s = sanitizeSuffix(name.toLowerCase());
    return s.isNotEmpty() ? s : juce::String("_export");
}

inline juce::var toVar(const Preset& p)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("name", p.name);
    o->setProperty("format", formatId(p.format));
    o->setProperty("bit_depth", static_cast<int>(p.bitDepth));
    o->setProperty("sample_rate", static_cast<int>(p.sampleRate));
    o->setProperty("mp3_bitrate", static_cast<int>(p.mp3Bitrate));
    o->setProperty("loudness", p.loudness);
    o->setProperty("suffix", p.suffix);
    return juce::var(o);
}

inline Preset fromVar(const juce::var& v)
{
    Preset p;
    p.name = v["name"].toString().trim();
    p.format = formatFromId(v["format"].toString());
    const int depth = static_cast<int>(v["bit_depth"]);
    p.bitDepth = depth == 16 ? BitDepth::Bits16 : depth == 32 ? BitDepth::Bits32 : BitDepth::Bits24;
    if (!AudioExportOptions::supportsBitDepth(p.format, p.bitDepth)) p.bitDepth = BitDepth::Bits24;
    const int rate = static_cast<int>(v["sample_rate"]);
    p.sampleRate = rate == 44100   ? SampleRate::SR44100
                   : rate == 48000 ? SampleRate::SR48000
                   : rate == 96000 ? SampleRate::SR96000
                                   : SampleRate::Source;
    const int kbps = static_cast<int>(v["mp3_bitrate"]);
    p.mp3Bitrate = kbps == 128 ? Mp3Bitrate::kbps128 : kbps == 192 ? Mp3Bitrate::kbps192 : Mp3Bitrate::kbps320;
    p.loudness = v["loudness"].toString();
    p.suffix = sanitizeSuffix(v["suffix"].toString());
    if (p.suffix.isEmpty()) p.suffix = suffixForName(p.name);
    return p;
}

/// The user's presets in `json` (the preference value), sorted by name;
/// nameless entries are dropped.
inline std::vector<Preset> parse(const juce::String& json)
{
    std::vector<Preset> out;
    const auto parsed = juce::JSON::parse(json); // keep it alive while iterating
    if (auto* arr = parsed.getArray())
        for (const auto& v : *arr) {
            auto p = fromVar(v);
            if (p.name.isNotEmpty()) out.push_back(std::move(p));
        }
    std::sort(out.begin(), out.end(),
              [](const Preset& a, const Preset& b) { return a.name.compareIgnoreCase(b.name) < 0; });
    return out;
}

inline juce::String serialize(const std::vector<Preset>& presets)
{
    juce::Array<juce::var> arr;
    for (const auto& p : presets)
        if (!p.builtIn() && p.name.isNotEmpty()) arr.add(toVar(p));
    return juce::JSON::toString(juce::var(arr), true);
}

/// `presets` with `p` added, replacing one of the same name (case-insensitive).
inline std::vector<Preset> withPreset(std::vector<Preset> presets, Preset p)
{
    p.name = p.name.trim();
    p.builtInId = 0;
    p.suffix = sanitizeSuffix(p.suffix);
    if (p.suffix.isEmpty()) p.suffix = suffixForName(p.name);
    presets.erase(std::remove_if(presets.begin(), presets.end(),
                                 [&p](const Preset& q) { return q.name.equalsIgnoreCase(p.name); }),
                  presets.end());
    presets.push_back(std::move(p));
    std::sort(presets.begin(), presets.end(),
              [](const Preset& a, const Preset& b) { return a.name.compareIgnoreCase(b.name) < 0; });
    return presets;
}

inline std::vector<Preset> withoutPreset(std::vector<Preset> presets, const juce::String& name)
{
    presets.erase(std::remove_if(presets.begin(), presets.end(),
                                 [&name](const Preset& q) { return q.name.equalsIgnoreCase(name.trim()); }),
                  presets.end());
    return presets;
}

/// Where an extra preset's file goes: beside `primaryOutput`, named after the
/// original (`originalPath`'s name) with the preset's suffix and extension.
inline juce::String outputPathFor(const juce::String& primaryOutput, const juce::String& originalPath, const Preset& p)
{
    const juce::File dir = juce::File(primaryOutput).getParentDirectory();
    juce::String base = juce::File::createLegalFileName(juce::File(originalPath).getFileNameWithoutExtension()).trim();
    if (base.isEmpty()) base = "export";
    return dir.getChildFile(base + p.suffix + export_plan::extensionFor(p.format)).getFullPathName();
}

/// Every extra file's path for one export: unique among themselves and never
/// the primary output (a clash gets "_2", "_3"...).
inline std::vector<juce::String> plannedOutputs(const juce::String& primaryOutput, const juce::String& originalPath,
                                                const std::vector<Preset>& extras)
{
    std::vector<juce::String> out;
    std::set<juce::String> used{juce::File(primaryOutput).getFullPathName().toLowerCase()};
    for (const auto& p : extras) {
        juce::File f(outputPathFor(primaryOutput, originalPath, p));
        const auto stem = f.getFileNameWithoutExtension();
        for (int n = 2; used.count(f.getFullPathName().toLowerCase()) != 0; ++n)
            f = f.getSiblingFile(stem + "_" + juce::String(n) + f.getFileExtension());
        used.insert(f.getFullPathName().toLowerCase());
        out.push_back(f.getFullPathName());
    }
    return out;
}

} // namespace khensu::export_presets
