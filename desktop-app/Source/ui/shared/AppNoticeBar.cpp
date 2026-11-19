// desktop-app/Source/ui/shared/AppNoticeBar.cpp
#include "AppNoticeBar.h"

namespace khensu {

AppNoticeBar::AppNoticeBar()
{
    applyThemeColours();
    message_.setFont(KhensuTheme::uiFont(12.0f));
    message_.setJustificationType(juce::Justification::centredLeft);
    message_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(message_);
    actionButton_.onClick = [this] { triggerAction(); };
    addChildComponent(actionButton_);
    dismissButton_.setTooltip(TRANS("Close this notice"));
    dismissButton_.onClick = [this] { triggerDismiss(); };
    addAndMakeVisible(dismissButton_);
    setVisible(false);
}

void AppNoticeBar::applyThemeColours()
{
    message_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    for (auto* b : {&actionButton_, &dismissButton_}) {
        b->setColour(juce::TextButton::buttonColourId, KhensuTheme::bgOverlay);
        b->setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    }
}

void AppNoticeBar::post(Notice notice)
{
    for (const auto& queued : queue_)
        if (notice.id.isNotEmpty() && queued.id == notice.id) return;
    queue_.push_back(std::move(notice));
    if (queue_.size() == 1) showFront();
}

void AppNoticeBar::withdraw(const juce::String& id)
{
    if (id.isEmpty()) return;
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->id != id) continue;
        const bool front = it == queue_.begin();
        queue_.erase(it);
        if (front) showFront();
        return;
    }
}

void AppNoticeBar::triggerAction()
{
    if (queue_.empty()) return;
    auto action = queue_.front().onAction; // copy: popFront() destroys the notice
    popFront();
    if (action) action();
}

void AppNoticeBar::triggerDismiss()
{
    if (queue_.empty()) return;
    auto dismiss = queue_.front().onDismiss;
    popFront();
    if (dismiss) dismiss();
}

void AppNoticeBar::popFront()
{
    queue_.pop_front();
    showFront();
}

void AppNoticeBar::showFront()
{
    const bool wasVisible = isVisible();
    if (queue_.empty()) {
        setVisible(false);
    } else {
        const auto& n = queue_.front();
        message_.setText(n.text, juce::dontSendNotification);
        setTitle(n.text);
        actionButton_.setButtonText(n.actionLabel);
        actionButton_.setTooltip(n.actionLabel);
        actionButton_.setVisible(n.actionLabel.isNotEmpty());
        setVisible(true);
        resized();
        repaint();
    }
    if (wasVisible != isVisible() && onVisibilityChanged) onVisibilityChanged();
}

void AppNoticeBar::paint(juce::Graphics& g)
{
    g.fillAll(KhensuTheme::bgRaised);
    g.setColour(KhensuTheme::accentTeal);
    g.fillRect(getLocalBounds().removeFromLeft(3));
    g.setColour(KhensuTheme::borderSubtle);
    g.fillRect(getLocalBounds().removeFromBottom(1));
}

void AppNoticeBar::resized()
{
    auto b = getLocalBounds().reduced(8, 3);
    dismissButton_.setBounds(b.removeFromRight(80));
    if (actionButton_.isVisible()) {
        b.removeFromRight(6);
        actionButton_.setBounds(b.removeFromRight(110));
    }
    b.removeFromRight(8);
    message_.setBounds(b);
}

} // namespace khensu
