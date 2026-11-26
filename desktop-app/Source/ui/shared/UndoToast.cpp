// desktop-app/Source/ui/shared/UndoToast.cpp
#include "UndoToast.h"
#include "ButtonRole.h"

namespace khensu {

UndoToast* UndoToast::find(juce::Component& host)
{
    for (auto* child : host.getChildren())
        if (auto* toast = dynamic_cast<UndoToast*>(child)) return toast;
    return nullptr;
}

UndoToast* UndoToast::show(juce::Component& host, const juce::String& message, std::function<void()> onUndo)
{
    if (auto* existing = find(host)) existing->close();
    // Owned by itself: deleted on timeout, Undo, Close or the host's deletion.
    auto* toast = new UndoToast(host, message, std::move(onUndo));
    return toast;
}

UndoToast::UndoToast(juce::Component& host, const juce::String& message, std::function<void()> onUndo)
    : host_(&host), message_(message), onUndo_(std::move(onUndo))
{
    applyThemeColours();
    setTitle(message_);
    setDescription(TRANS("Notification"));
    setAccessible(true);
    setWantsKeyboardFocus(false);
    setAlwaysOnTop(true);

    undoButton_.setTitle(TRANS("Undo") + ": " + message_);
    undoButton_.setTooltip(TRANS("Undo this change"));
    undoButton_.onClick = [this] { triggerUndo(); };
    button_role::markPrimary(undoButton_);
    if (onUndo_)
        addAndMakeVisible(undoButton_);
    else
        addChildComponent(undoButton_);

    closeButton_.setTitle(TRANS("Close notification"));
    closeButton_.setTooltip(TRANS("Close this notification"));
    closeButton_.onClick = [this] { close(); };
    addAndMakeVisible(closeButton_);

    host.addAndMakeVisible(this);
    host.addComponentListener(this);
    placeInHost();
    toFront(false);

    juce::AccessibilityHandler::postAnnouncement(onUndo_ ? message_ + ". " + TRANS("Undo is available.") : message_,
                                                 juce::AccessibilityHandler::AnnouncementPriority::medium);
    startTimer(kLifetimeMs);
}

UndoToast::~UndoToast()
{
    stopTimer();
    if (host_ != nullptr) {
        host_->removeComponentListener(this);
        host_->removeChildComponent(this);
    }
}

void UndoToast::applyThemeColours()
{
    closeButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgOverlay);
    closeButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
}

int UndoToast::preferredWidth() const
{
    const auto font = KhensuTheme::bodyFont();
    juce::GlyphArrangement ga;
    ga.addLineOfText(font, message_, 0.0f, 0.0f);
    const int textW = juce::roundToInt(ga.getBoundingBox(0, -1, true).getWidth());
    const int buttons = (onUndo_ ? 72 + KhensuTheme::sp2 : 0) + 64;
    return textW + buttons + KhensuTheme::sp4 * 2 + KhensuTheme::sp3;
}

void UndoToast::placeInHost()
{
    if (host_ == nullptr) return;
    const auto area = host_->getLocalBounds();
    const int w = juce::jlimit(juce::jmin(200, area.getWidth()), juce::jmax(0, area.getWidth() - 32), preferredWidth());
    const int y = juce::jmax(0, area.getHeight() - kBottomMargin - kHeight);
    setBounds(area.getCentreX() - w / 2, y, w, kHeight);
}

void UndoToast::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(KhensuTheme::shadowColour);
    g.fillRoundedRectangle(r.translated(0.0f, 2.0f), 8.0f);
    g.setColour(KhensuTheme::bgOverlay);
    g.fillRoundedRectangle(r, 8.0f);
    g.setColour(KhensuTheme::borderActive);
    g.drawRoundedRectangle(r, 8.0f, 1.0f);

    auto text = getLocalBounds().reduced(KhensuTheme::sp4, 0);
    text.removeFromRight(closeButton_.getWidth() +
                         (undoButton_.isVisible() ? undoButton_.getWidth() + KhensuTheme::sp2 : 0) + KhensuTheme::sp3);
    g.setColour(KhensuTheme::textPrimary);
    g.setFont(KhensuTheme::bodyFont());
    g.drawFittedText(message_, text, juce::Justification::centredLeft, 1);
}

void UndoToast::resized()
{
    auto b = getLocalBounds().reduced(KhensuTheme::sp2, (kHeight - KhensuTheme::buttonHSmall) / 2);
    closeButton_.setBounds(b.removeFromRight(64));
    if (undoButton_.isVisible()) {
        b.removeFromRight(KhensuTheme::sp2);
        undoButton_.setBounds(b.removeFromRight(72));
    }
}

void UndoToast::mouseEnter(const juce::MouseEvent&) { stopTimer(); }

void UndoToast::mouseExit(const juce::MouseEvent& e)
{
    if (!getLocalBounds().contains(e.getEventRelativeTo(this).getPosition())) startTimer(kLifetimeMs / 2);
}

void UndoToast::timerCallback() { close(); }

void UndoToast::componentMovedOrResized(juce::Component&, bool, bool) { placeInHost(); }

void UndoToast::componentBeingDeleted(juce::Component& c)
{
    // The host is going: go with it without running the undo (its state is
    // being torn down too).
    c.removeComponentListener(this);
    host_ = nullptr;
    finished_ = true;
    stopTimer();
    delete this;
}

void UndoToast::triggerUndo() { finish(true); }
void UndoToast::close() { finish(false); }

void UndoToast::finish(bool undo)
{
    if (finished_) return;
    finished_ = true;
    auto action = undo ? std::move(onUndo_) : std::function<void()>{};
    delete this; // a Button may delete its parent from onClick (BailOutChecker)
    if (action) action();
}

} // namespace khensu
