// desktop-app/Source/ui/shared/PanelDockState.h
// Persistent layout state for dockable panels: which panels are popped out
// into their own windows (and where), and user-resized panel heights.
// Pure data with JSON persistence - unit-tested in tests/ui/PanelDockTest.cpp.
#pragma once
#include <JuceHeader.h>
#include <map>
#include <optional>
#include <string>

namespace khensu {

class PanelDockState {
  public:
    static constexpr int kMinHeight = 120;
    static constexpr int kMaxHeight = 1200;

    bool isPoppedOut(const std::string& id) const { return popped_.count(id) > 0; }
    std::optional<juce::Rectangle<int>> windowBounds(const std::string& id) const
    {
        const auto it = popped_.find(id);
        if (it == popped_.end() || it->second.isEmpty()) return std::nullopt;
        return it->second;
    }

    void setPoppedOut(const std::string& id, juce::Rectangle<int> windowBounds = {}) { popped_[id] = windowBounds; }
    void setDocked(const std::string& id) { popped_.erase(id); }
    void dockAll() { popped_.clear(); }
    std::vector<std::string> poppedOutIds() const
    {
        std::vector<std::string> ids;
        for (const auto& [id, b] : popped_)
            ids.push_back(id);
        return ids;
    }

    /// The user-dragged height for `id`, else `fallback`.
    int heightFor(const std::string& id, int fallback) const
    {
        const auto it = heights_.find(id);
        return it == heights_.end() ? fallback : it->second;
    }
    void setHeight(const std::string& id, int h) { heights_[id] = juce::jlimit(kMinHeight, kMaxHeight, h); }
    void resetHeight(const std::string& id) { heights_.erase(id); }
    bool hasCustomHeight(const std::string& id) const { return heights_.count(id) > 0; }

    juce::var toVar() const
    {
        auto* root = new juce::DynamicObject();
        auto* popped = new juce::DynamicObject();
        for (const auto& [id, b] : popped_)
            popped->setProperty(juce::Identifier(id), b.toString());
        auto* heights = new juce::DynamicObject();
        for (const auto& [id, h] : heights_)
            heights->setProperty(juce::Identifier(id), h);
        root->setProperty("popped_out", juce::var(popped));
        root->setProperty("heights", juce::var(heights));
        return juce::var(root);
    }

    void fromVar(const juce::var& v)
    {
        popped_.clear();
        heights_.clear();
        if (auto* popped = v["popped_out"].getDynamicObject())
            for (const auto& p : popped->getProperties())
                popped_[p.name.toString().toStdString()] = juce::Rectangle<int>::fromString(p.value.toString());
        if (auto* heights = v["heights"].getDynamicObject())
            for (const auto& p : heights->getProperties())
                if (p.value.isInt() || p.value.isDouble() || p.value.isInt64())
                    setHeight(p.name.toString().toStdString(), static_cast<int>(p.value));
    }

  private:
    std::map<std::string, juce::Rectangle<int>> popped_;
    std::map<std::string, int> heights_;
};

} // namespace khensu
