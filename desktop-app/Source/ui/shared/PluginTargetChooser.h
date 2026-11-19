// desktop-app/Source/ui/shared/PluginTargetChooser.h
// "Target: All instances / <instance id>" - which Copilot plugin instance a
// desktop parameter push addresses (PLG-04, desktop half). The service keys
// /transport/params by instance; with several Copilots loaded (vocal bus,
// drum bus, master) an untargeted push changes them all.
//
// Header-only and decoupled from AppController through three callbacks, so
// the item logic is unit-tested (tests/ui/OrphanRouteFeaturesTest.cpp). The
// list is rebuilt from the latest playhead poll each time the popup opens.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <utility>
#include <vector>

namespace khensu {

class PluginTargetChooser final : public juce::ComboBox {
  public:
    using InstancesFn = std::function<std::vector<juce::String>()>;
    using GetTargetFn = std::function<juce::String()>;
    using SetTargetFn = std::function<void(const juce::String&)>;

    PluginTargetChooser(InstancesFn instances, GetTargetFn getTarget, SetTargetFn setTarget)
        : instances_(std::move(instances)), getTarget_(std::move(getTarget)), setTarget_(std::move(setTarget))
    {
        setTitle(TRANS("Plugin target"));
        setTooltip(TRANS("Which Copilot plugin instance receives pushed parameters: all instances, or one."));
        onChange = [this] {
            const int idx = getSelectedId() - 1;
            if (idx >= 0 && idx < static_cast<int>(ids_.size()) && setTarget_) setTarget_(ids_[static_cast<size_t>(idx)]);
        };
        refresh();
    }

    /// Rebuilds the items from the known instances and selects the current
    /// target. A target no longer reported stays listed as "(not seen)" so
    /// the choice is never silently reset to All.
    void refresh()
    {
        const auto items = buildItems(instances_ ? instances_() : std::vector<juce::String>{},
                                      getTarget_ ? getTarget_() : juce::String());
        clear(juce::dontSendNotification);
        ids_.clear();
        int id = 1;
        for (const auto& [instanceId, label] : items) {
            addItem(label, id++);
            ids_.push_back(instanceId);
        }
        const juce::String target = getTarget_ ? getTarget_() : juce::String();
        for (size_t i = 0; i < ids_.size(); ++i)
            if (ids_[i] == target) setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);
    }

    void showPopup() override
    {
        refresh();
        juce::ComboBox::showPopup();
    }

    /// (instance id, label) rows: "" = all first, then each known id, then a
    /// current target that is not (or no longer) reported.
    static std::vector<std::pair<juce::String, juce::String>> buildItems(const std::vector<juce::String>& instances,
                                                                         const juce::String& currentTarget)
    {
        std::vector<std::pair<juce::String, juce::String>> items;
        items.emplace_back(juce::String(), TRANS("Target: All instances"));
        bool targetListed = currentTarget.isEmpty();
        for (const auto& id : instances) {
            if (id.isEmpty()) continue;
            bool dup = false;
            for (const auto& it : items)
                dup = dup || it.first == id;
            if (dup) continue;
            items.emplace_back(id, TRANS("Target:") + " " + id);
            targetListed = targetListed || id == currentTarget;
        }
        if (!targetListed) items.emplace_back(currentTarget, TRANS("Target:") + " " + currentTarget + " " + TRANS("(not seen)"));
        return items;
    }

  private:
    InstancesFn instances_;
    GetTargetFn getTarget_;
    SetTargetFn setTarget_;
    std::vector<juce::String> ids_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginTargetChooser)
};

} // namespace khensu
