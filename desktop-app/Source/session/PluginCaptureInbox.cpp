// desktop-app/Source/session/PluginCaptureInbox.cpp
#include "PluginCaptureInbox.h"
#include <khensu/session/PluginCapture.h>

namespace khensu::capture_inbox {

std::vector<Imported> importPending(SessionManager& db, const juce::File& dir)
{
    std::vector<Imported> out;
    for (const auto& manifestFile : plugin_capture::pending(dir)) {
        const auto manifest = plugin_capture::fromJson(manifestFile.loadFileAsString());
        const auto audio = manifest ? dir.getChildFile(manifest->audioFile) : juce::File();
        if (!manifest || !audio.existsAsFile()) {
            plugin_capture::markImported(manifestFile); // nothing to import; don't retry it every scan
            continue;
        }
        // The followed session's target (genre, intent): the capture is
        // graded as part of that track.
        CreativeTarget target;
        if (manifest->followedSessionId.isNotEmpty())
            if (auto followed = db.loadSession(manifest->followedSessionId.toStdString());
                followed.is_ok() && followed.value())
                target = followed.value()->target;
        const auto name = manifest->name.isNotEmpty() ? manifest->name : audio.getFileNameWithoutExtension();
        auto created = db.createSession(name.toStdString(), target);
        if (created.is_err()) continue; // the database is busy: the next scan tries again
        Session session = created.value();
        AudioFileRef f;
        f.id = juce::Uuid().toDashedString().toStdString();
        f.path = audio.getFullPathName().toStdString();
        f.format = "wav";
        f.sample_rate = static_cast<int>(manifest->sampleRate);
        f.channels = manifest->channels;
        f.duration_seconds = manifest->seconds;
        f.bit_depth = 32;
        f.title = name.toStdString();
        session.files = {f};
        if (db.saveSession(session).is_err()) {
            db.deleteSession(session.id);
            continue;
        }
        plugin_capture::markImported(manifestFile);
        out.push_back({session.id, name});
    }
    return out;
}

} // namespace khensu::capture_inbox
