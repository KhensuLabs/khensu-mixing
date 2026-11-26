// plugin-copilot/Source/ui/PluginSegmentedTabs.h
// VIS-9: the editor's top-level sections as a segmented control - the look
// of the desktop's SegmentedControl (a raised track, the selected segment a
// teal-tinted pill with a rim, hairline dividers) - instead of JUCE's
// TabbedComponent. Each segment is a real button, so Tab reaches every one
// and a screen reader names it; Left/Right on a focused segment moves the
// selection. Pages are children shown one at a time.
// THREAD_CONTEXT(UIThread)
#pragma once

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <vector>

namespace khensu {

class PluginSegmentedTabs final : public juce::Component {
  public:
    static constexpr int kBarHeight = 34;

    PluginSegmentedTabs();
    ~PluginSegmentedTabs() override;

    /// Adds a section (not owned). The first one added is selected.
    void addTab(const juce::String& name, juce::Component& page);
    int getNumTabs() const noexcept { return static_cast<int>(pages_.size()); }
    juce::StringArray getTabNames() const;
    int getCurrentTabIndex() const noexcept { return current_; }
    /// Selects section `index` (out of range: ignored) and calls onTabChanged.
    void setCurrentTabIndex(int index);
    /// The segment button of section `index` (nullptr when out of range).
    juce::Button* getSegmentButton(int index) const;

    std::function<void(int)> onTabChanged;

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    class Segment;
    juce::Rectangle<int> barBounds() const;

    std::vector<std::unique_ptr<Segment>> segments_;
    std::vector<juce::Component*> pages_;
    int current_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginSegmentedTabs)
};

} // namespace khensu
