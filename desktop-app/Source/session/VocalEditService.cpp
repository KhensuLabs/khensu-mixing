// desktop-app/Source/session/VocalEditService.cpp
// Moved from AppController.h / AppController_Session.cpp (ENG-1 stage C3);
// behaviour unchanged.
#include "VocalEditService.h"
#include <khensu/common/EventHub.h>

namespace khensu {

void VocalEditService::setSelectedVocalStrategy(const juce::String& s) noexcept
{
    vocalStrategySet_ = true; // any explicit set (user / undo / autosave restore)
    if (selectedVocalStrategy_ == s) return;
    if (!isUndoingRedoing_) pushUndoState();
    selectedVocalStrategy_ = s;
    EventHub::getInstance().publish({EventType::VocalStrategyChanged, s});
    if (onChanged) onChanged();
}

float VocalEditService::getVocalMacroValue(const juce::String& name) const noexcept
{
    auto it = vocalMacroValues_.find(name);
    if (it != vocalMacroValues_.end()) return it->second;
    return 0.0f;
}

void VocalEditService::setVocalMacroValue(const juce::String& name, float value) noexcept
{
    if (vocalMacroValues_[name] == value) return;
    vocalMacroValues_[name] = value;
    EventHub::getInstance().publish({EventType::VocalMacroChanged, name, value});
    if (onChanged) onChanged();
}

void VocalEditService::pushUndoState()
{
    if (isUndoingRedoing_) return;
    undoHistory_.push({selectedVocalStrategy_, vocalMacroValues_});
}

void VocalEditService::undo()
{
    if (!undoHistory_.canUndo()) return;
    isUndoingRedoing_ = true;
    auto state = undoHistory_.undo({selectedVocalStrategy_, vocalMacroValues_});
    setSelectedVocalStrategy(state.strategy);
    for (const auto& [name, val] : state.macros) {
        setVocalMacroValue(name, val);
    }
    isUndoingRedoing_ = false;
}

void VocalEditService::redo()
{
    if (!undoHistory_.canRedo()) return;
    isUndoingRedoing_ = true;
    auto state = undoHistory_.redo({selectedVocalStrategy_, vocalMacroValues_});
    setSelectedVocalStrategy(state.strategy);
    for (const auto& [name, val] : state.macros) {
        setVocalMacroValue(name, val);
    }
    isUndoingRedoing_ = false;
}

void VocalEditService::restoreStrategy(const juce::String& strategy)
{
    selectedVocalStrategy_ = strategy;
    vocalStrategySet_ = true; // restored counts as explicit
    EventHub::getInstance().publish({EventType::VocalStrategyChanged, selectedVocalStrategy_});
}

void VocalEditService::restoreMacros(const juce::DynamicObject& saved)
{
    for (const auto& [name, val] : vocalMacroValues_) {
        if (saved.hasProperty(name)) {
            vocalMacroValues_[name] = static_cast<float>(static_cast<double>(saved.getProperty(name)));
            EventHub::getInstance().publish({EventType::VocalMacroChanged, name, vocalMacroValues_[name]});
        }
    }
}

void VocalEditService::resetForSession()
{
    takes_.clear();
    notifyTakes();
    undoHistory_.clear();
    selectedVocalStrategy_ = "clean";
    vocalStrategySet_ = false;
    vocalMacroValues_ = defaultVocalMacros();
}

bool VocalEditService::mergeTakes(const juce::StringArray& paths)
{
    bool added = false;
    for (const auto& path : paths) {
        if (path.isEmpty() || takes_.contains(path) || !juce::File(path).existsAsFile()) continue;
        takes_.add(path);
        added = true;
    }
    return added;
}

void VocalEditService::notifyTakes()
{
    // A copy: a listener may deregister itself (a panel destroyed by the
    // change it is told about).
    const auto listeners = takesListeners_;
    for (const auto& [key, cb] : listeners)
        if (takesListeners_.count(key) != 0 && cb) cb();
}

void VocalEditService::addSessionTakes(const juce::StringArray& paths)
{
    if (!mergeTakes(paths)) return;
    notifyTakes();
    if (onChanged) onChanged();
}

void VocalEditService::removeSessionTake(const juce::String& path)
{
    if (!takes_.contains(path)) return;
    takes_.removeString(path);
    notifyTakes();
    if (onChanged) onChanged();
}

void VocalEditService::restoreTakes(const juce::StringArray& paths)
{
    takes_.clear();
    mergeTakes(paths);
    notifyTakes();
}

} // namespace khensu
