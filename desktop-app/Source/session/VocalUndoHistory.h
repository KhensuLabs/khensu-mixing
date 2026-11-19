// desktop-app/Source/session/VocalUndoHistory.h
#pragma once
#include <JuceHeader.h>
#include <map>
#include <vector>

namespace khensu {

/// Undo/redo stack for the vocal workspace's strategy + macro edits, extracted
/// from AppController (audit M4 — second slice of the SessionState
/// decomposition). Pure state: it stores snapshots and moves them between the
/// undo and redo stacks, but never applies one. AppController keeps the apply
/// logic (which publishes events and autosaves) and the `isUndoingRedoing_`
/// re-entrancy guard, exactly as before the extraction.
class VocalUndoHistory {
  public:
    struct Snapshot {
        juce::String strategy;
        std::map<juce::String, float> macros;
    };

    bool canUndo() const noexcept { return !undo_.empty(); }
    bool canRedo() const noexcept { return !redo_.empty(); }

    /// Record `current` as an undo point. Drops the oldest snapshot past
    /// kMaxDepth and clears the redo tail (a fresh edit invalidates the redo
    /// future — standard undo semantics).
    void push(const Snapshot& current)
    {
        if (undo_.size() >= kMaxDepth) undo_.erase(undo_.begin());
        undo_.push_back(current);
        redo_.clear();
    }

    /// Precondition: canUndo(). Moves `current` onto the redo stack and returns
    /// the snapshot the caller should restore.
    Snapshot undo(const Snapshot& current)
    {
        redo_.push_back(current);
        Snapshot s = undo_.back();
        undo_.pop_back();
        return s;
    }

    /// Precondition: canRedo(). Mirror of undo().
    Snapshot redo(const Snapshot& current)
    {
        undo_.push_back(current);
        Snapshot s = redo_.back();
        redo_.pop_back();
        return s;
    }

    void clear()
    {
        undo_.clear();
        redo_.clear();
    }

    size_t undoDepth() const noexcept { return undo_.size(); }
    size_t redoDepth() const noexcept { return redo_.size(); }

  private:
    static constexpr size_t kMaxDepth = 50;
    std::vector<Snapshot> undo_, redo_;
};

} // namespace khensu
