// desktop-app/Source/ui/shared/ServiceHealthWidget.h
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h"
#include "SidebarComponent.h"
#include "ServiceStatusText.h"

namespace khensu {

class ServiceHealthWidget final : public juce::Button, private juce::Timer {
  public:
    ServiceHealthWidget() : juce::Button("service-health-widget")
    {
        startTimerHz(5); // poll status 5 times a second
    }

    ~ServiceHealthWidget() override { stopTimer(); }

    std::function<SidebarComponent::PythonStatus()> getStatus;
    std::function<void()> onRestartRequested;
    std::function<void()> onCopyLogsRequested;
    std::function<void()> onDiagnoseRequested;

    void clicked() override
    {
        juce::PopupMenu menu;
        menu.addItem(1, TRANS("Restart analysis engine"));
        menu.addItem(2, TRANS("Copy analysis engine log to clipboard"));
        menu.addItem(3, TRANS("Diagnose analysis engine"));

        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [this](int result) {
            if (result == 1 && onRestartRequested) {
                onRestartRequested();
            } else if (result == 2 && onCopyLogsRequested) {
                onCopyLogsRequested();
            } else if (result == 3 && onDiagnoseRequested) {
                onDiagnoseRequested();
            }
        });
    }

    void paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override
    {
        auto b = getLocalBounds().toFloat();

        // Hover styling
        if (isMouseOverButton || isButtonDown) {
            g.setColour(KhensuTheme::bgOverlay.withAlpha(isButtonDown ? 0.9f : 0.6f));
            g.fillRoundedRectangle(b, 4.0f);
        }

        // Draw dot
        const auto status = getStatus ? getStatus() : SidebarComponent::PythonStatus::Stopped;
        juce::Colour dotCol = KhensuTheme::textTertiary;
        // UX-7: the sidebar's wording, shortened so a header never
        // ellipsizes it (UX2-3); the full line is the tooltip.
        const juce::String labelText = service_status::shortStateLine(status);
        if (const auto tip = service_status::stateLine(status); getTooltip() != tip) setTooltip(tip);

        if (status == SidebarComponent::PythonStatus::Running)
            dotCol = KhensuTheme::accentTeal;
        else if (status == SidebarComponent::PythonStatus::Starting)
            dotCol = KhensuTheme::textSecondary; // on its way: a hollow ring, not a warning
        else if (status == SidebarComponent::PythonStatus::GaveUp)
            dotCol = KhensuTheme::accentRed;

        auto dotArea = b.removeFromLeft(16).withSizeKeepingCentre(6, 6);
        g.setColour(dotCol);
        if (status == SidebarComponent::PythonStatus::Starting)
            g.drawEllipse(dotArea.reduced(0.5f), 1.0f);
        else
            g.fillEllipse(dotArea);

        // Draw label
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(labelFont());
        g.drawText(labelText, b, juce::Justification::centredLeft, false);
    }

    /// The width the widest state needs, for the host's layout.
    static int preferredWidth()
    {
        int w = 0;
        for (auto s : {SidebarComponent::PythonStatus::Running, SidebarComponent::PythonStatus::Starting,
                       SidebarComponent::PythonStatus::GaveUp, SidebarComponent::PythonStatus::Stopped})
            w = juce::jmax(w, juce::GlyphArrangement::getStringWidthInt(labelFont(), service_status::shortStateLine(s)));
        return w + 16 + 8;
    }

  private:
    static juce::Font labelFont() { return KhensuTheme::labelFont(KhensuTheme::kFontLabel); }
    void timerCallback() override { repaint(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ServiceHealthWidget)
};

} // namespace khensu
