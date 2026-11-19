// shared-core/include/khensu/session/PluginCapture.h
// FEAT-7: a capture from the plugin, handed to the desktop app. The plugin
// never writes khensu.db (PLG-10), so it writes the audio and a manifest into
// <data dir>/captures/; the desktop app, which owns the database, turns each
// pending manifest into a session and offers to open it.
// THREAD_CONTEXT(AnyThread) - file and JSON helpers.
#pragma once
#include <juce_core/juce_core.h>
#include <khensu/common/AppPaths.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace khensu::plugin_capture {

/// A pending capture's manifest: "<stem>.capture.json" beside "<stem>.wav".
inline constexpr const char* kPendingSuffix = ".capture.json";
/// Imported manifests are renamed to this, so they are never imported twice.
inline constexpr const char* kImportedSuffix = ".imported.json";

struct Manifest {
    juce::String audioFile;         ///< the WAV's file name, in the same folder
    juce::String name;              ///< the session name to give it
    juce::String genre;             ///< the followed session's genre id ("" = none)
    juce::String followedSessionId; ///< the desktop session the plugin followed, if any
    double bpm = 0.0;
    double sampleRate = 0.0;
    int channels = 2;
    double seconds = 0.0;
    juce::String createdAt; ///< ISO 8601
};

inline juce::File capturesDirectory()
{
    auto dir = juce::File(juce::String(AppPaths::dataDirectory())).getChildFile("captures");
    if (!dir.isDirectory()) dir.createDirectory();
    return dir;
}

inline juce::String toJson(const Manifest& m)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty("version", 1);
    o->setProperty("audio_file", m.audioFile);
    o->setProperty("name", m.name);
    o->setProperty("genre", m.genre);
    o->setProperty("followed_session_id", m.followedSessionId);
    o->setProperty("bpm", m.bpm);
    o->setProperty("sample_rate", m.sampleRate);
    o->setProperty("channels", m.channels);
    o->setProperty("seconds", m.seconds);
    o->setProperty("created_at", m.createdAt);
    return juce::JSON::toString(juce::var(o.get()));
}

inline std::optional<Manifest> fromJson(const juce::String& text)
{
    const auto v = juce::JSON::parse(text);
    if (!v.isObject() || static_cast<int>(v["version"]) != 1) return std::nullopt;
    Manifest m;
    m.audioFile = v["audio_file"].toString();
    // Only a bare file name: a manifest never points outside its folder.
    if (m.audioFile.isEmpty() || m.audioFile.containsAnyOf("/\\:") || m.audioFile.contains("..")) return std::nullopt;
    m.name = v["name"].toString();
    m.genre = v["genre"].toString();
    m.followedSessionId = v["followed_session_id"].toString();
    m.bpm = static_cast<double>(v["bpm"]);
    m.sampleRate = static_cast<double>(v["sample_rate"]);
    m.channels = static_cast<int>(v["channels"]);
    m.seconds = static_cast<double>(v["seconds"]);
    m.createdAt = v["created_at"].toString();
    return m;
}

/// Pending manifests in `dir`, oldest first.
inline juce::Array<juce::File> pending(const juce::File& dir)
{
    auto files = dir.findChildFiles(juce::File::findFiles, false, juce::String("*") + kPendingSuffix);
    std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) {
        return a.getLastModificationTime() < b.getLastModificationTime();
    });
    return files;
}

/// Renames a pending manifest to its imported name; false when it could not.
inline bool markImported(const juce::File& manifest)
{
    const auto name = manifest.getFileName();
    if (!name.endsWith(kPendingSuffix)) return false;
    return manifest.moveFileTo(
        manifest.getSiblingFile(name.dropLastCharacters((int)std::strlen(kPendingSuffix)) + kImportedSuffix));
}

/// Samples in `bars` bars of 4/4 at `bpm` and `sampleRate`; 0 when unknown.
inline juce::int64 samplesForBars(int bars, double bpm, double sampleRate)
{
    if (bars <= 0 || bpm <= 0.0 || sampleRate <= 0.0) return 0;
    return static_cast<juce::int64>(std::llround(bars * 4.0 * 60.0 / bpm * sampleRate));
}

/// Samples in `beats` beats (a loop region's length) at `bpm`.
inline juce::int64 samplesForBeats(double beats, double bpm, double sampleRate)
{
    if (beats <= 0.0 || bpm <= 0.0 || sampleRate <= 0.0) return 0;
    return static_cast<juce::int64>(std::llround(beats * 60.0 / bpm * sampleRate));
}

} // namespace khensu::plugin_capture
