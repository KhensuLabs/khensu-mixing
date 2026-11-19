// desktop-app/Source/ui/SessionIntake/IntakeQueueState.h
// The intake analysis queue shared between the message thread and the
// queue worker (audit 2026-11-06 DSK-01).
//
// The worker used to loop over IntakeScreen::probedFiles_ and look up
// cancelFlags_ directly - containers the message thread clears, reorders and
// erases from while the user drags rows or drops new files - and it
// dereferenced a SafePointer off the message thread. That is a data race:
// torn strings, out-of-range indexes, the wrong file analysed, crashes. And a
// second drop started a second worker over the same vector.
//
// Now the message thread owns the UI containers and mirrors every change into
// this mutex-guarded snapshot of {path, duration, cancel flag}; the worker
// only ever talks to this object. Each drop starts a new GENERATION: the
// previous worker sees it is stale on its next step, cancels its in-flight
// job and exits.
// THREAD_CONTEXT(AnyThread) - every member is guarded by mutex_.
#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace khensu {

class IntakeQueueState final {
  public:
    struct Entry {
        juce::String path;
        double durationSeconds = 0.0;
        std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
    };

    /// Message thread: a new set of files. Returns the new generation; every
    /// worker of an older generation stops (and its entries are cancelled).
    int reset(std::vector<Entry> entries)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& e : entries_)
            e.cancelled->store(true);
        entries_ = std::move(entries);
        processed_.clear();
        return ++generation_;
    }

    /// Message thread: the user reordered rows (the new full order of paths).
    void reorder(const std::vector<juce::String>& orderedPaths)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Entry> next;
        next.reserve(entries_.size());
        for (const auto& p : orderedPaths)
            for (const auto& e : entries_)
                if (e.path == p) {
                    next.push_back(e);
                    break;
                }
        // Anything not named keeps its relative order at the end.
        for (const auto& e : entries_) {
            bool named = false;
            for (const auto& n : next)
                named = named || n.path == e.path;
            if (!named) next.push_back(e);
        }
        entries_ = std::move(next);
    }

    /// Message thread: the user cancelled (removed) one row.
    void cancel(const juce::String& path)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (it->path == path) {
                it->cancelled->store(true);
                entries_.erase(it);
                return;
            }
    }

    /// Worker: the next entry to analyse in the CURRENT order, or nothing
    /// when the queue is done or `generation` is stale. Marks it processed.
    std::optional<Entry> takeNext(int generation)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_) return std::nullopt;
        for (const auto& e : entries_) {
            if (processed_.count(e.path) != 0 || e.cancelled->load()) continue;
            processed_.insert(e.path);
            return e;
        }
        return std::nullopt;
    }

    bool isCurrent(int generation) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation == generation_;
    }

    /// Number of entries still queued (for "analyzing i/n").
    int size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return (int)entries_.size();
    }

    /// Position of `path` in the current order, -1 if gone.
    int indexOf(const juce::String& path) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].path == path) return (int)i;
        return -1;
    }

  private:
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::set<juce::String> processed_;
    int generation_ = 0;
};

} // namespace khensu
