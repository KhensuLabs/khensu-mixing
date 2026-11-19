// desktop-app/Source/ui/shared/PanelDockManager.cpp
#include "PanelDockManager.h"
#include "KhensuTheme.h"

namespace khensu {

class PanelDockManager::FloatingWindow final : public juce::DocumentWindow {
  public:
    FloatingWindow(const juce::String& title, std::function<void()> onClose, std::function<void()> onMoved)
        : juce::DocumentWindow(title + " - Khensu", KhensuTheme::bgBase,
                               juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          onClose_(std::move(onClose)), onMoved_(std::move(onMoved))
    {
        setUsingNativeTitleBar(true);
        setResizable(true, false);
        setResizeLimits(240, 160, 4000, 3000);
    }
    void closeButtonPressed() override
    {
        if (onClose_) onClose_();
    }
    void moved() override
    {
        juce::DocumentWindow::moved();
        if (onMoved_) onMoved_();
    }
    void resized() override
    {
        juce::DocumentWindow::resized();
        if (onMoved_) onMoved_();
    }

  private:
    std::function<void()> onClose_, onMoved_;
};

PanelDockManager::PanelDockManager(std::function<void()> onLayoutChanged, std::function<void()> onStateChanged)
    : onLayoutChanged_(std::move(onLayoutChanged)), onStateChanged_(std::move(onStateChanged))
{}

PanelDockManager::~PanelDockManager()
{
    // Return every panel to its owner before the windows die, so no panel is
    // destroyed while parented to a window it doesn't own. Don't re-layout or
    // persist here - the owner is being torn down and the popped-out state
    // should survive to the next launch.
    for (auto& [id, e] : windows_) {
        if (e.window != nullptr) e.window->clearContentComponent();
        if (e.panel != nullptr && e.originalParent != nullptr)
            e.originalParent->addChildComponent(e.panel.getComponent());
    }
    windows_.clear();
}

void PanelDockManager::popOut(const std::string& id, juce::Component& panel, const juce::String& title)
{
    if (isPoppedOut(id)) {
        windows_[id].window->toFront(true);
        return;
    }
    Entry e;
    e.panel = &panel;
    e.originalParent = panel.getParentComponent();
    const auto size = panel.getBounds().isEmpty() ? juce::Rectangle<int>(0, 0, 520, 360) : panel.getLocalBounds();

    e.window = std::make_unique<FloatingWindow>(
        title,
        [this, id, alive = std::weak_ptr<int>(alive_)] {
            // Deferred: the window is still inside its own closeButtonPressed().
            juce::MessageManager::callAsync([this, id, alive] {
                if (!alive.expired()) dock(id);
            });
        },
        [this, id] {
            auto it = windows_.find(id);
            // State only - moves fire per pixel; the owner persists state()
            // on dock/pop-out and at shutdown.
            if (it != windows_.end() && it->second.window != nullptr)
                state_.setPoppedOut(id, it->second.window->getBounds());
        });
    if (e.originalParent != nullptr) e.originalParent->removeChildComponent(&panel);
    panel.setVisible(true);
    e.window->setContentNonOwned(&panel, false);
    if (const auto saved = state_.windowBounds(id))
        e.window->setBounds(*saved);
    else
        e.window->centreWithSize(juce::jmax(320, size.getWidth()), juce::jmax(220, size.getHeight() + 30));
    e.window->setVisible(true);
    windows_[id] = std::move(e);
    state_.setPoppedOut(id, windows_[id].window->getBounds());
    notifyState();
    if (onLayoutChanged_) onLayoutChanged_();
}

void PanelDockManager::dock(const std::string& id)
{
    auto it = windows_.find(id);
    if (it == windows_.end()) return;
    auto entry = std::move(it->second);
    windows_.erase(it);
    if (entry.window != nullptr) entry.window->clearContentComponent();
    if (entry.panel != nullptr && entry.originalParent != nullptr)
        entry.originalParent->addAndMakeVisible(entry.panel.getComponent());
    entry.window.reset();
    state_.setDocked(id);
    notifyState();
    if (onLayoutChanged_) onLayoutChanged_();
}

void PanelDockManager::dockAll()
{
    std::vector<std::string> ids;
    for (const auto& [id, e] : windows_)
        ids.push_back(id);
    for (const auto& id : ids)
        dock(id);
    state_.dockAll();
    notifyState();
}

void PanelDockManager::notifyState()
{
    if (onStateChanged_) onStateChanged_();
}

} // namespace khensu
