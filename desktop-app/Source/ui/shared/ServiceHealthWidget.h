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
        // UX-7: the sidebar's wording, not a fifth variant of it.
        const juce::String labelText = service_status::stateLine(status);

        if (status == SidebarComponent::PythonStatus::Running)
            dotCol = KhensuTheme::accentTeal;
        else if (status == SidebarComponent::PythonStatus::Starting)
            dotCol = KhensuTheme::accentAmber;
        else if (status == SidebarComponent::PythonStatus::GaveUp)
            dotCol = KhensuTheme::accentRed;

        auto dotArea = b.removeFromLeft(14).withSizeKeepingCentre(6, 6);
        g.setColour(dotCol);
        g.fillEllipse(dotArea);

        // Draw label
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(khensu::KhensuTheme::uiFont(10.5f, juce::Font::plain));
        g.drawText(labelText, b, juce::Justification::centredLeft, true);
    }

  private:
    void timerCallback() override { repaint(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ServiceHealthWidget)
};

} // namespace khensu
