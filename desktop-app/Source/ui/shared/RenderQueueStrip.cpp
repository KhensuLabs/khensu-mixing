// desktop-app/Source/ui/shared/RenderQueueStrip.cpp
#include "RenderQueueStrip.h"
#include "KhensuLookAndFeel.h"
#include "KhensuTheme.h"

namespace khensu {

RenderQueueStrip::RenderQueueStrip(RenderJobManager& jobs) : jobs_(jobs)
{
    jobs_.addChangeListener(this);
    setVisible(false);
}

RenderQueueStrip::~RenderQueueStrip() { jobs_.removeChangeListener(this); }

int RenderQueueStrip::preferredHeight() const
{
    return shownJobs_.empty() ? 0 : static_cast<int>(shownJobs_.size()) * kRowH + 2;
}

void RenderQueueStrip::changeListenerCallback(juce::ChangeBroadcaster*) { rebuildRows(); }

// The cancel buttons are rebuilt per job, so this walks the live vector rather
// than naming fixed members - a theme switch over a static queue would
// otherwise never reach them (UI-01).
void RenderQueueStrip::applyThemeColours()
{
    for (const auto& btn : cancelButtons_)
        if (btn) btn->setColour(juce::TextButton::buttonColourId, KhensuTheme::accentRed.darker(0.6f));
}

void RenderQueueStrip::rebuildRows()
{
    shownJobs_ = jobs_.activeJobs();

    // One cancel button per row, recreated on every change - job counts are
    // tiny (a handful at most), so simplicity beats reuse here.
    cancelButtons_.clear();
    for (const auto& job : shownJobs_) {
        auto btn = std::make_unique<juce::TextButton>("Cancel");
        // Cancellable from the moment the job is submitted - RenderJobManager
        // keys cancellation off localId, which is valid immediately, so the
        // /start-in-flight window is no longer a dead-control gap
        // (frontend-review-2026-07-17.md P1).
        const juce::uint32 localId = job.localId;
        btn->onClick = [this, localId] { jobs_.cancelJob(localId); };
        addAndMakeVisible(*btn);
        cancelButtons_.push_back(std::move(btn));
    }
    applyThemeColours();

    const bool shouldShow = !shownJobs_.empty();
    if (shouldShow != isVisible()) {
        setVisible(shouldShow);
        if (shouldShow)
            startTimer(1000);
        else
            stopTimer();
        if (onVisibilityChanged) onVisibilityChanged();
    } else if (shouldShow && onVisibilityChanged) {
        onVisibilityChanged(); // row count may have changed -> owner re-layouts
    }
    resized();
    repaint();
}

void RenderQueueStrip::paint(juce::Graphics& g)
{
    g.fillAll(KhensuTheme::bgRaised);
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.35f));
    g.fillRect(0, 0, getWidth(), 1);

    const auto now = juce::Time::currentTimeMillis();
    for (size_t i = 0; i < shownJobs_.size(); ++i) {
        auto row = juce::Rectangle<int>(0, 1 + (int)i * kRowH, getWidth(), kRowH).reduced(10, 2);
        row.removeFromRight(70 + 8); // cancel button + gap

        const auto& job = shownJobs_[i];
        const int elapsedSec = (int)((now - job.startedMs) / 1000);

        g.setColour(KhensuTheme::accentTeal);
        g.setFont(juce::Font(juce::FontOptions{12.f, juce::Font::bold}));
        auto label = "Rendering: " + job.description;
        g.drawText(label, row.removeFromLeft(juce::jmax(100, getWidth() * 2 / 3)), juce::Justification::centredLeft,
                   true);

        g.setColour(KhensuTheme::textSecondary);
        g.setFont(juce::Font(juce::FontOptions{11.f}));
        g.drawText("(" + job.status + ", " + juce::String(elapsedSec) + "s)", row, juce::Justification::centredLeft,
                   true);
    }
}

void RenderQueueStrip::resized()
{
    for (size_t i = 0; i < cancelButtons_.size(); ++i) {
        auto row = juce::Rectangle<int>(0, 1 + (int)i * kRowH, getWidth(), kRowH).reduced(6, 3);
        cancelButtons_[i]->setBounds(row.removeFromRight(70));
    }
}

} // namespace khensu
