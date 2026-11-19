// desktop-app/Source/session/LufsCache.h
#pragma once
#include <JuceHeader.h>
#include <map>
#include <optional>

namespace khensu {

/// Integrated LUFS per audio file for the loudness-matched A/B (AUDIT-038).
/// Keyed by path and validated against the file's size and modification time,
/// so a render rewritten in place under the same name is never matched with
/// the old level (AUDIT-018). Message-thread only; pure state + file stat.
class LufsCache final {
  public:
    std::optional<float> find(const juce::String& path) const
    {
        const auto it = entries_.find(path);
        if (it == entries_.end()) return std::nullopt;
        const juce::File f(path);
        if (!f.existsAsFile() || f.getSize() != it->second.sizeBytes ||
            f.getLastModificationTime() != it->second.modified)
            return std::nullopt;
        return it->second.lufs;
    }

    /// Records the file's current level. No-op for a missing file.
    void remember(const juce::String& path, float lufs)
    {
        const juce::File f(path);
        if (!f.existsAsFile()) return;
        entries_[path] = Entry{lufs, f.getSize(), f.getLastModificationTime()};
    }

    void clear() noexcept { entries_.clear(); }

  private:
    struct Entry {
        float lufs = 0.0f;
        juce::int64 sizeBytes = 0;
        juce::Time modified;
    };
    std::map<juce::String, Entry> entries_;
};

} // namespace khensu
