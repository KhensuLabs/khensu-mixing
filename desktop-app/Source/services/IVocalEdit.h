// desktop-app/Source/services/IVocalEdit.h
#pragma once
#include <JuceHeader.h>

namespace khensu {

/// The Vocal Workspace's editable state (ENG-1): strategy, macros and their
/// undo/redo, and the session's Takes bin (WF-5). Autosaved per session.
/// Message thread.
class IVocalEdit {
  public:
    virtual ~IVocalEdit() = default;

    virtual juce::String getSelectedVocalStrategy() const noexcept = 0;
    virtual void setSelectedVocalStrategy(const juce::String& strategy) noexcept = 0;
    /// True once a strategy was chosen or restored this run.
    virtual bool hasExplicitVocalStrategy() const noexcept = 0;
    virtual float getVocalMacroValue(const juce::String& name) const noexcept = 0;
    virtual void setVocalMacroValue(const juce::String& name, float value) noexcept = 0;

    virtual void pushUndoState() = 0;
    virtual void undo() = 0;
    virtual void redo() = 0;
    virtual bool canUndo() const noexcept = 0;
    virtual bool canRedo() const noexcept = 0;

    /// The session's Takes bin (WF-5): the vocal takes loaded once and shared
    /// by Comping, Take Alignment and Preflight, in the order added.
    virtual juce::StringArray sessionTakes() const = 0;
    /// Adds existing audio files to the bin (duplicates and missing files are
    /// skipped); notifies the takes listeners and autosaves when it changed.
    virtual void addSessionTakes(const juce::StringArray& paths) = 0;
    virtual void removeSessionTake(const juce::String& path) = 0;
    virtual void addTakesListener(void* key, std::function<void()> cb) = 0;
    virtual void removeTakesListener(void* key) = 0;
};

} // namespace khensu
