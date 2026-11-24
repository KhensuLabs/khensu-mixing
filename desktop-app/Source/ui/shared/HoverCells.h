// desktop-app/Source/ui/shared/HoverCells.h
// VIZ-U3: hover readouts for the charts that draw their own data (not on the
// chart toolkit): while painting, a chart records each mark's area and what
// it says ("250 Hz: +2.1 dB vs reference"); on a mouse move the readout of
// the area under the pointer becomes the tooltip. One small helper, so every
// chart reads out its exact values.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <utility>
#include <vector>

namespace khensu {

class HoverCells {
  public:
    void clear() { cells_.clear(); }
    void add(juce::Rectangle<float> area, juce::String text) { cells_.emplace_back(area, std::move(text)); }
    /// A column-shaped cell (x0..x1 across `bounds`' full height).
    void addColumn(float x0, float x1, juce::Rectangle<float> bounds, juce::String text)
    {
        add({std::min(x0, x1), bounds.getY(), std::max(2.0f, std::abs(x1 - x0)), bounds.getHeight()}, std::move(text));
    }
    /// The readout at `p` (the last-added cell wins where they overlap), or "".
    juce::String at(juce::Point<float> p) const
    {
        for (auto it = cells_.rbegin(); it != cells_.rend(); ++it)
            if (it->first.contains(p)) return it->second;
        return {};
    }
    bool empty() const noexcept { return cells_.empty(); }

    /// Call from mouseMove: sets the readout as `client`'s tooltip.
    void update(juce::SettableTooltipClient& client, juce::Point<float> p, const juce::String& fallback = {}) const
    {
        const auto text = at(p);
        client.setTooltip(text.isNotEmpty() ? text : fallback);
    }

  private:
    std::vector<std::pair<juce::Rectangle<float>, juce::String>> cells_;
};

} // namespace khensu
