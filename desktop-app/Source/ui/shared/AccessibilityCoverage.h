// desktop-app/Source/ui/shared/AccessibilityCoverage.h
// Guarantees every interactive control has a spoken name and a tooltip.
//
// apply() walks a component tree and fills only what is MISSING - explicit
// titles/tooltips set by a panel always win:
//   * name    <- button text, an attached/labelling juce::Label, the
//                component name, or the tooltip (in that order); symbol-only
//                button text ("+", "?", "<", ...) is expanded to words.
//   * tooltip <- the description, else the derived name (+ an attached
//                label's text for sliders/combos).
// audit() reports controls still missing either, for tests and diagnostics.
// Cheap enough to re-run whenever a screen is shown, so dynamically built
// rows are covered too.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <vector>

namespace khensu::AccessibilityCoverage {

struct Gap {
    juce::Component* component = nullptr;
    bool missingName = false;
    bool missingTooltip = false;
    juce::String describe() const;
};

/// True for components a user operates (buttons, sliders, combo boxes,
/// text editors, list boxes).
bool isInteractive(const juce::Component& c);

/// Fills missing names/tooltips under `root` (inclusive). Returns how many
/// properties were filled.
int apply(juce::Component& root);

/// Interactive, showing-or-not controls under `root` still lacking a name
/// or tooltip.
std::vector<Gap> audit(juce::Component& root);

/// Readable words for symbol-only labels ("+" -> "Increase"), or empty.
juce::String wordsForSymbol(const juce::String& text);

} // namespace khensu::AccessibilityCoverage
