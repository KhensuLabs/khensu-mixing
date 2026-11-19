// desktop-app/Source/ui/VocalWorkspace/VocalStrategyPanel.cpp
#include "VocalStrategyPanel.h"
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"
#include <khensu/common/EventHub.h>

namespace khensu {

// Strategy segment IDs in index order (matches addSegment calls below)
static const char* kSegmentIds[] = {"clean", "intimate", "breathy", "aggressive", "ethereal"};
static constexpr int kSegmentCount = 5;

VocalStrategyPanel::VocalStrategyPanel(IAppController& c) : PanelCard(c, "Vocal Strategy")
{
    setExplainer(TRANS("Vocal Strategy"), TRANS("Choose the vocal production approach that fits your track. "
                                   "The system auto-suggests a mode based on your Creative Target genre. "
                                   "Changing strategy updates the Vocal Macros panel."));

    strategyControl_.addSegment("Clean");
    strategyControl_.addSegment("Intimate");
    strategyControl_.addSegment("Breathy");
    strategyControl_.addSegment("Aggressive");
    strategyControl_.addSegment("Ethereal");
    strategyControl_.onChange = [this](int idx) {
        if (idx >= 0 && idx < kSegmentCount) {
            if (onStrategyChanged) onStrategyChanged(juce::String(kSegmentIds[idx]));
        }
    };
    addAndMakeVisible(strategyControl_);

    EventHub::getInstance().addListener(this, EventType::VocalStrategyChanged, [this](const EventData& data) {
        juce::String target = data.stringVal;
        int idx = -1;
        for (int i = 0; i < kSegmentCount; ++i) {
            if (juce::String(kSegmentIds[i]) == target) {
                idx = i;
                break;
            }
        }
        if (idx >= 0 && strategyControl_.getSelected() != idx) {
            strategyControl_.setSelected(idx);
        }
    });
}

VocalStrategyPanel::~VocalStrategyPanel() { EventHub::getInstance().removeListener(this); }

void VocalStrategyPanel::setData(const AnalysisResult&)
{
    auto* session = controller_.getActiveSession();
    if (session == nullptr) return;

    // Only suggest the genre default when the user hasn't made (or restored)
    // a choice yet. This ran unconditionally before, so every visit to the
    // Vocal Workspace clobbered the user's strategy + macros back to the
    // genre default and pushed spurious undo/autosave entries.
    if (controller_.vocalEdit().hasExplicitVocalStrategy()) {
        // Reflect the existing choice in the control without re-firing events.
        const juce::String current = controller_.vocalEdit().getSelectedVocalStrategy();
        for (int i = 0; i < kSegmentCount; ++i) {
            if (current == kSegmentIds[i]) {
                if (strategyControl_.getSelected() != i) strategyControl_.setSelected(i);
                return;
            }
        }
        return;
    }

    const int idx = suggestSegmentIndex(session->target.primary_genre);
    strategyControl_.setSelected(idx);
    if (onStrategyChanged) onStrategyChanged(juce::String(kSegmentIds[idx]));
}

juce::String VocalStrategyPanel::selectedStrategyId() const
{
    const int idx = strategyControl_.getSelected();
    if (idx >= 0 && idx < kSegmentCount) return kSegmentIds[idx];
    return "clean";
}

void VocalStrategyPanel::paint(juce::Graphics& g) { PanelCard::paint(g); }

void VocalStrategyPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    strategyControl_.setBounds(b.removeFromTop(32));
}

/*static*/ int VocalStrategyPanel::suggestSegmentIndex(const std::string& g) noexcept
{
    // 0=clean, 1=intimate, 2=breathy, 3=aggressive, 4=ethereal
    if (g == "trap" || g == "drill" || g == "melodic-rap" || g == "emo-rap" || g == "boom-bap" || g == "phonk" ||
        g == "rap" || g == "grime" || g == "metal" || g == "metalcore" || g == "alt-rock" || g == "grunge" ||
        g == "doom-metal" || g == "death-metal" || g == "black-metal" || g == "nu-metal" || g == "brostep" ||
        g == "dubstep" || g == "hardcore-punk" || g == "post-hardcore" || g == "punk" || g == "math-rock" ||
        g == "noise-rock" || g == "stoner-rock" || g == "riddim")
        return 3; // aggressive

    if (g == "jazz" || g == "acoustic" || g == "singer-songwriter" || g == "electro-swing" || g == "lo-fi-beats" ||
        g == "lo-fi-hip-hop" || g == "bedroom-pop")
        return 1; // intimate

    if (g == "dream-pop" || g == "chillwave" || g == "chillstep" || g == "trance" || g == "liquid-drum-and-bass" ||
        g == "melodic-dubstep" || g == "future-bass" || g == "ambient" || g == "synthwave" || g == "post-rock" ||
        g == "shoegaze" || g == "cloud-rap" || g == "vaporwave" || g == "retrowave")
        return 4; // ethereal

    if (g == "r-b" || g == "soul" || g == "funk" || g == "afrobeats" || g == "neo-soul") return 2; // breathy

    // Default: clean (pop, house, dance, etc.)
    return 0;
}

} // namespace khensu
