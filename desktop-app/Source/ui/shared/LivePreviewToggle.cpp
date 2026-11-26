// desktop-app/Source/ui/shared/LivePreviewToggle.cpp
// STU-9: see LivePreviewToggle.h.
#include "LivePreviewToggle.h"
#include "../../IAppController.h"

namespace khensu {

void LivePreviewToggle::applyThemeColours()
{
    note_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    toggle_.setColour(juce::ToggleButton::textColourId, KhensuTheme::textPrimary);
    toggle_.setColour(juce::ToggleButton::tickColourId, KhensuTheme::accentTeal);
}

LivePreviewToggle::LivePreviewToggle(IAppController& controller, juce::String label, ChainProvider provider)
    : controller_(controller), label_(std::move(label)), provider_(std::move(provider))
{
    applyThemeColours();
    toggle_.setButtonText(TRANS("Preview live"));
    toggle_.setTitle(TRANS("Preview live"));
    toggle_.setTooltip(TRANS("Hear the edit while you make it: what is playing runs through this chain, matched "
                             "to the source's loudness. Nothing is rendered or saved until you apply it."));
    toggle_.onClick = [this] { setPreviewing(toggle_.getToggleState()); };
    addAndMakeVisible(toggle_);

    note_.setFont(KhensuTheme::uiFont(khensu::type::caption));
    note_.setMinimumHorizontalScale(0.8f);
    note_.setTitle(TRANS("Live preview status"));
    addAndMakeVisible(note_);
}

LivePreviewToggle::~LivePreviewToggle()
{
    stopTimer();
    // Only this surface's own preview; the controller outlives its screens.
    if (toggle_.getToggleState()) controller_.playback().clearLivePreview(this);
}

void LivePreviewToggle::setPreviewing(bool on)
{
    toggle_.setToggleState(on, juce::dontSendNotification);
    if (on) {
        push();
        startTimer(250);
    } else {
        stopTimer();
        controller_.playback().clearLivePreview(this);
        refreshNote();
    }
}

void LivePreviewToggle::chainChanged()
{
    if (toggle_.getToggleState()) push();
}

void LivePreviewToggle::push()
{
    controller_.playback().setLivePreviewChain(this, provider_ ? provider_() : decltype(provider_())(), label_);
    refreshNote();
}

void LivePreviewToggle::refreshNote()
{
    if (!toggle_.getToggleState()) {
        note_.setText({}, juce::dontSendNotification);
        note_.setTooltip({});
        return;
    }
    const auto status = controller_.playback().livePreviewStatus();
    juce::String text;
    if (status.live.isEmpty() && status.substituted.isEmpty())
        text = status.skipped.isEmpty() ? TRANS("Nothing to preview yet - make an edit.")
                                        : TRANS("This chain can only be heard once rendered.");
    else if (!status.skipped.isEmpty())
        text = TRANS("Previewing - some stages are heard only after rendering.");
    else if (!status.substituted.isEmpty())
        text = TRANS("Previewing - some stages with a live stand-in.");
    else
        text = TRANS("Previewing, level matched.");
    note_.setText(text, juce::dontSendNotification);
    juce::StringArray detail;
    if (!status.live.isEmpty()) detail.add(TRANS("Live:") + " " + status.live.joinIntoString(", "));
    for (const auto& s : status.substituted)
        detail.add(s);
    for (const auto& s : status.skipped)
        detail.add(s);
    note_.setTooltip(detail.joinIntoString("\n"));
}

void LivePreviewToggle::timerCallback()
{
    if (!toggle_.getToggleState()) {
        stopTimer();
        return;
    }
    // Cleared underneath (a session change, another surface's preview) or
    // this surface left the screen: stop.
    if (!controller_.playback().isLivePreviewOwner(this) || !isShowing()) setPreviewing(false);
}

void LivePreviewToggle::resized()
{
    auto b = getLocalBounds();
    toggle_.setBounds(b.removeFromLeft(juce::jmin(130, b.getWidth())));
    b.removeFromLeft(KhensuTheme::sp1);
    note_.setBounds(b);
}

} // namespace khensu
