// desktop-app/Source/session/BounceWatcher.h
// ENH-1 "Watch the bounce folder": notices a new bounce of the open session's
// track so the app can analyse it as a new version and check the previous
// findings against it.
//
// The rule (documented in docs/manual.md by the lead):
//  * The session's file itself (on by default): when its size or modification
//    time changes - the DAW bounced over it - that new content is a new bounce.
//  * Its folder (off by default): a newer audio file whose name is the
//    session file's name with a version tail - "song_v2.wav", "song 3.wav",
//    "song-take4.flac", "song (2).wav" for "song.wav" or "song_v1.wav" - and
//    whose modification time is newer than the session file's (and than the
//    last bounce taken). The app's own renders ("<name>_fix_<stamp>.wav",
//    isAppRenderFileFor) and files already in the render tree never count.
//  * Debounce: a bounce is taken only once it is non-empty, its size and
//    modification time have not changed for kQuietMs (the writer has
//    stopped), and it opens for reading (the writer has closed it).
//
// Portable (a JUCE timer poll, every kPollMs; no ReadDirectoryChangesW), so
// it builds on macOS too. Message thread. The pure parts are unit-tested.
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <map>

namespace khensu {

namespace bounce_watch {

struct FileStat {
    bool exists = false;
    juce::int64 size = 0;
    juce::int64 modifiedMs = 0;
    bool operator==(const FileStat& o) const noexcept
    {
        return exists == o.exists && size == o.size && modifiedMs == o.modifiedMs;
    }
    bool operator!=(const FileStat& o) const noexcept { return !(*this == o); }
};

FileStat statOf(const juce::File& f);

/// "song_v2" -> "song", "Song (3)" -> "song", "song-take4" -> "song",
/// "song 2" -> "song", "song" -> "song": the name without its version tail,
/// lower-cased. A name that is all digits keeps them.
juce::String baseStem(const juce::String& nameWithoutExtension);

/// True when `candidate` is another bounce of `sessionFile` by the folder
/// rule's name test: a supported audio file in the same folder, not the file
/// itself, not one of the app's renders, with the same baseStem().
bool isSiblingBounce(const juce::File& candidate, const juce::File& sessionFile);

/// Waits for a file to settle: Ready once it exists, is non-empty and its
/// stat has not changed for `quietMs`. Fed one observation per poll.
class Debouncer {
  public:
    enum class State { Settling, Ready };
    explicit Debouncer(juce::int64 quietMs = 3000) : quietMs_(quietMs) {}
    State observe(const FileStat& stat, juce::int64 nowMs);
    void reset() { started_ = false; }

  private:
    juce::int64 quietMs_;
    bool started_ = false;
    FileStat last_;
    juce::int64 sinceMs_ = 0;
};

} // namespace bounce_watch

class BounceWatcher final : private juce::Timer {
  public:
    static constexpr int kPollMs = 2000;
    static constexpr juce::int64 kQuietMs = 3000;

    struct Settings {
        bool watchFile = true;    ///< the session's file bounced over in place
        bool watchFolder = false; ///< a newer "<name>_v2" beside it
    };

    BounceWatcher();
    ~BounceWatcher() override;

    /// A settled new bounce: `bounce` is the file; `inPlace` when it is the
    /// session's own file with new content (else a sibling by the folder rule).
    std::function<void(const juce::File& bounce, bool inPlace)> onNewBounce;
    /// The file to watch, asked on every poll (the active session's original;
    /// "" when none). A different answer re-baselines without an event.
    std::function<juce::String()> sourceProvider;
    /// Paths to ignore in the folder rule (the render tree's versions).
    std::function<bool(const juce::File&)> isKnownVersion;

    void setSettings(Settings s);
    Settings settings() const noexcept { return settings_; }
    /// Starts / stops the poll timer.
    void start();
    void stop();

    /// One poll (the timer's; tests call it with their own clock).
    void poll(juce::int64 nowMs);
    /// Test seam: whether a settled file opens for reading (default: it does
    /// when a FileInputStream opens it).
    std::function<bool(const juce::File&)> canRead;
    /// The watched file (for tests / diagnostics).
    const juce::File& watchedFile() const noexcept { return watched_; }

  private:
    void timerCallback() override { poll(juce::Time::currentTimeMillis()); }
    void rebaseline(const juce::File& file);
    bool readable(const juce::File& f) const;

    Settings settings_;
    juce::File watched_;
    bounce_watch::FileStat baseline_; ///< the session file as last taken
    bounce_watch::Debouncer fileDebounce_{kQuietMs};
    bool fileChanging_ = false;
    juce::int64 folderSinceMs_ = 0; ///< only siblings modified after this count
    struct Candidate {
        bounce_watch::Debouncer debounce{kQuietMs};
    };
    std::map<juce::String, Candidate> candidates_; ///< by path

    JUCE_DECLARE_NON_COPYABLE(BounceWatcher)
};

} // namespace khensu
