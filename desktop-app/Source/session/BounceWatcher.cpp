// desktop-app/Source/session/BounceWatcher.cpp
#include "BounceWatcher.h"
#include <regex>
#include "../ui/shared/AppFileDrop.h" // isSupportedAudio
#include "../ui/shared/RenderPaths.h"              // isAppRenderFileFor

namespace khensu {

namespace bounce_watch {

FileStat statOf(const juce::File& f)
{
    FileStat s;
    s.exists = f.existsAsFile();
    if (!s.exists) return s;
    s.size = f.getSize();
    s.modifiedMs = f.getLastModificationTime().toMilliseconds();
    return s;
}

juce::String baseStem(const juce::String& nameWithoutExtension)
{
    // A version tail needs a separator or a version word, so "track01" and
    // "track02" stay different songs while "song_v2" / "song 2" / "song-take3"
    // / "song (1)" all read as "song".
    static const std::regex kCopyTail(R"(\s*\(\d+\)$)");
    static const std::regex kVersionTail(
        R"(([ _.\-]+(v|ver|version|take|tk|rev|mix|bounce)?[ _.\-]?\d+|(v|ver|version|take|tk|rev)\d+)$)");
    std::string s = nameWithoutExtension.trim().toLowerCase().toStdString();
    for (int i = 0; i < 3; ++i) {
        const std::string before = s;
        std::string stripped = std::regex_replace(s, kCopyTail, "");
        stripped = std::regex_replace(stripped, kVersionTail, "");
        if (!stripped.empty()) s = stripped; // an all-digit name keeps its digits
        if (s == before) break;
    }
    return juce::String(juce::CharPointer_UTF8(s.c_str())).trim();
}

bool isSiblingBounce(const juce::File& candidate, const juce::File& sessionFile)
{
    if (candidate == sessionFile || candidate.getParentDirectory() != sessionFile.getParentDirectory()) return false;
    if (!app_file_drop::isSupportedAudio(candidate)) return false;
    if (isAppRenderFileFor(candidate, sessionFile)) return false;
    const auto base = baseStem(sessionFile.getFileNameWithoutExtension());
    return base.isNotEmpty() && baseStem(candidate.getFileNameWithoutExtension()) == base;
}

Debouncer::State Debouncer::observe(const FileStat& stat, juce::int64 nowMs)
{
    if (!started_ || stat != last_) {
        started_ = true;
        last_ = stat;
        sinceMs_ = nowMs;
        return State::Settling;
    }
    if (!stat.exists || stat.size <= 0) return State::Settling;
    return nowMs - sinceMs_ >= quietMs_ ? State::Ready : State::Settling;
}

} // namespace bounce_watch

BounceWatcher::BounceWatcher() = default;

BounceWatcher::~BounceWatcher() { stopTimer(); }

void BounceWatcher::setSettings(Settings s)
{
    settings_ = s;
    // Whatever changed meanwhile is not a new bounce: start from now.
    if (watched_ != juce::File()) rebaseline(watched_);
}

void BounceWatcher::start() { startTimer(kPollMs); }

void BounceWatcher::stop()
{
    stopTimer();
    watched_ = juce::File();
    candidates_.clear();
}

void BounceWatcher::rebaseline(const juce::File& file)
{
    watched_ = file;
    baseline_ = bounce_watch::statOf(file);
    fileDebounce_.reset();
    fileChanging_ = false;
    candidates_.clear();
    // Siblings already there (older bounces, the file itself) are history:
    // only one written after all of them, and after now, counts.
    folderSinceMs_ = juce::jmax(baseline_.modifiedMs, juce::Time::currentTimeMillis());
    const auto dir = file.getParentDirectory();
    if (dir.isDirectory())
        for (const auto& entry : juce::RangedDirectoryIterator(dir, false, "*", juce::File::findFiles))
            if (bounce_watch::isSiblingBounce(entry.getFile(), file))
                folderSinceMs_ = juce::jmax(folderSinceMs_, entry.getModificationTime().toMilliseconds());
}

bool BounceWatcher::readable(const juce::File& f) const
{
    if (canRead) return canRead(f);
    juce::FileInputStream in(f);
    return in.openedOk();
}

void BounceWatcher::poll(juce::int64 nowMs)
{
    const juce::String path = sourceProvider ? sourceProvider() : juce::String();
    if (path.isEmpty()) {
        watched_ = juce::File();
        candidates_.clear();
        return;
    }
    const juce::File file(path);
    if (file != watched_) {
        rebaseline(file);
        return;
    }

    // ---- The session's own file, bounced over in place ----
    if (settings_.watchFile) {
        const auto now = bounce_watch::statOf(file);
        if (!fileChanging_ && now != baseline_) {
            fileChanging_ = true;
            fileDebounce_.reset();
        }
        if (fileChanging_ && fileDebounce_.observe(now, nowMs) == bounce_watch::Debouncer::State::Ready &&
            readable(file)) {
            fileChanging_ = false;
            fileDebounce_.reset();
            baseline_ = now;
            folderSinceMs_ = juce::jmax(folderSinceMs_, now.modifiedMs);
            if (onNewBounce) onNewBounce(file, true);
            return; // one bounce per poll
        }
    } else {
        baseline_ = bounce_watch::statOf(file); // off: changes meanwhile are not bounces
    }

    // ---- A newer sibling ("song_v2.wav") ----
    if (!settings_.watchFolder) return;
    const auto dir = file.getParentDirectory();
    if (!dir.isDirectory()) return;
    std::map<juce::String, bool> present;
    juce::File ready;
    juce::int64 readyMs = 0;
    for (const auto& entry : juce::RangedDirectoryIterator(dir, false, "*", juce::File::findFiles)) {
        const auto& f = entry.getFile();
        const auto mod = entry.getModificationTime().toMilliseconds();
        if (mod <= folderSinceMs_ || !bounce_watch::isSiblingBounce(f, file)) continue;
        if (isKnownVersion && isKnownVersion(f)) continue;
        const auto key = f.getFullPathName();
        present[key] = true;
        auto& candidate = candidates_[key];
        if (candidate.debounce.observe(bounce_watch::statOf(f), nowMs) == bounce_watch::Debouncer::State::Ready &&
            readable(f) && mod > readyMs) {
            ready = f;
            readyMs = mod;
        }
    }
    for (auto it = candidates_.begin(); it != candidates_.end();) // gone or taken: forget
        it = present.count(it->first) != 0 ? std::next(it) : candidates_.erase(it);
    if (ready == juce::File()) return;
    // The newest settled sibling is the bounce; older ones written meanwhile
    // are passed over with it.
    folderSinceMs_ = readyMs;
    candidates_.clear();
    if (onNewBounce) onNewBounce(ready, false);
}

} // namespace khensu
