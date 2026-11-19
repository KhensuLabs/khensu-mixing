// desktop-app/Source/session/VocalEditService.h
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <map>
#include "../services/IVocalEdit.h"
#include "VocalUndoHistory.h"

namespace khensu {

/// The Vocal Workspace's editable state (ENG-1 stage C3, extracted from
/// AppController): the selected vocal strategy, the macro values, and their
/// undo/redo (VocalUndoHistory + the isUndoingRedoing_ re-entrancy guard).
/// Changes are published on the EventHub; persistence stays with the owner,
/// which hooks onChanged to its autosave and restores through the owner API
/// below (restores never push undo). Message thread.
class VocalEditService final : public IVocalEdit {
  public:
    VocalEditService() = default;

    /// Fired after a user-visible strategy/macro change (not on a restore) so
    /// the owner persists the workspace (AppController::autosaveState).
    std::function<void()> onChanged;

    // ---- IVocalEdit ----
    juce::String getSelectedVocalStrategy() const noexcept override { return selectedVocalStrategy_; }
    void setSelectedVocalStrategy(const juce::String& s) noexcept override;
    /// True once a strategy has been explicitly chosen or restored this run -
    /// VocalStrategyPanel only applies the genre-default suggestion before that,
    /// so panel refreshes no longer clobber the user's choice.
    bool hasExplicitVocalStrategy() const noexcept override { return vocalStrategySet_; }
    float getVocalMacroValue(const juce::String& name) const noexcept override;
    void setVocalMacroValue(const juce::String& name, float value) noexcept override;

    void pushUndoState() override;
    void undo() override;
    void redo() override;
    bool canUndo() const noexcept override { return undoHistory_.canUndo(); }
    bool canRedo() const noexcept override { return undoHistory_.canRedo(); }

    juce::StringArray sessionTakes() const override { return takes_; }
    void addSessionTakes(const juce::StringArray& paths) override;
    void removeSessionTake(const juce::String& path) override;
    void addTakesListener(void* key, std::function<void()> cb) override { takesListeners_[key] = std::move(cb); }
    void removeTakesListener(void* key) override { takesListeners_.erase(key); }

    // ---- Owner API (AppController) ----
    static std::map<juce::String, float> defaultVocalMacros()
    {
        return {{"Compression", 75.0f}, {"Presence", 70.0f}, {"Width", 40.0f}, {"De-essing", 60.0f},
                {"Saturation", 20.0f},  {"Reverb", 30.0f},   {"Delay", 25.0f}};
    }
    /// Every macro's current value (autosave).
    const std::map<juce::String, float>& macroValues() const noexcept { return vocalMacroValues_; }
    /// Autosave restore: sets the strategy (counts as explicit) and publishes
    /// it, without pushing undo or firing onChanged.
    void restoreStrategy(const juce::String& strategy);
    /// Autosave restore: applies every known macro present in `saved`,
    /// publishing each, without pushing undo or firing onChanged.
    void restoreMacros(const juce::DynamicObject& saved);
    /// Autosave restore: the bin's takes that still exist, without firing
    /// onChanged.
    void restoreTakes(const juce::StringArray& paths);
    /// Session change: back to the default strategy and macros, no undo, an
    /// empty Takes bin.
    void resetForSession();

  private:
    juce::String selectedVocalStrategy_ = "clean";
    bool vocalStrategySet_ = false; ///< see hasExplicitVocalStrategy()
    std::map<juce::String, float> vocalMacroValues_ = defaultVocalMacros();

    // Vocal strategy/macro undo/redo stacks (VocalUndoHistory, audit M4).
    // isUndoingRedoing_ guards setSelectedVocalStrategy from re-pushing while
    // a restore is applying snapshots.
    juce::StringArray takes_;                              ///< the session's Takes bin (WF-5)
    std::map<void*, std::function<void()>> takesListeners_;
    /// Appends the existing, not yet listed files; true when any was added.
    bool mergeTakes(const juce::StringArray& paths);
    void notifyTakes();

    VocalUndoHistory undoHistory_;
    bool isUndoingRedoing_ = false;

    JUCE_DECLARE_NON_COPYABLE(VocalEditService)
};

} // namespace khensu
