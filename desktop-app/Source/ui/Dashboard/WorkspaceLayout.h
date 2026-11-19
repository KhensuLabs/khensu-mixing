// desktop-app/Source/ui/Dashboard/WorkspaceLayout.h
#pragma once
#include <JuceHeader.h>
#include <vector>
#include <map>
#include <string>

namespace khensu {

/// Serializable Dashboard workspace layout: Zone 3 panel order + per-panel
/// visibility + report mode. Backs both "User profiles" (Engineer/Producer/
/// Content Creator) and "Workspace/layout modes" (Analysis-Heavy/Creative-
/// Heavy/Mastering-Focused) as named built-in bundles, plus arbitrary
/// user-saved "Dashboard Layout" presets - all persisted the same way via
/// SessionManager::setUserPreference/getUserPreference under a
/// "dashboard_layout_<name>" key.
struct WorkspaceLayout {
    std::vector<std::string> panelOrder;         // zone3 panel ids, in display order
    std::map<std::string, bool> panelVisibility; // id -> visible (missing id = visible)
    juce::String reportMode = "engineer";        // "client" or "engineer"

    juce::String toJson() const
    {
        auto* obj = new juce::DynamicObject();
        juce::Array<juce::var> orderArr;
        for (const auto& id : panelOrder)
            orderArr.add(juce::String(id));
        obj->setProperty("panel_order", orderArr);

        auto* visObj = new juce::DynamicObject();
        for (const auto& [id, vis] : panelVisibility)
            visObj->setProperty(juce::String(id), vis);
        obj->setProperty("panel_visibility", juce::var(visObj));

        obj->setProperty("report_mode", reportMode);
        return juce::JSON::toString(juce::var(obj));
    }

    static WorkspaceLayout fromJson(const juce::String& json)
    {
        WorkspaceLayout layout;
        auto parsed = juce::JSON::parse(json);
        auto* obj = parsed.getDynamicObject();
        if (obj == nullptr) return layout;

        if (auto* orderArr = obj->getProperty("panel_order").getArray())
            for (const auto& v : *orderArr)
                layout.panelOrder.push_back(v.toString().toStdString());

        if (auto* visObj = obj->getProperty("panel_visibility").getDynamicObject())
            for (const auto& prop : visObj->getProperties())
                layout.panelVisibility[prop.name.toString().toStdString()] = static_cast<bool>(prop.value);

        auto rm = obj->getProperty("report_mode").toString();
        if (rm.isNotEmpty()) layout.reportMode = rm;
        return layout;
    }

    static WorkspaceLayout makeBuiltIn(const std::vector<std::string>& order, const std::vector<std::string>& hidden,
                                       const juce::String& reportMode)
    {
        WorkspaceLayout layout;
        layout.panelOrder = order;
        for (const auto& id : hidden)
            layout.panelVisibility[id] = false;
        layout.reportMode = reportMode;
        return layout;
    }
};

} // namespace khensu
