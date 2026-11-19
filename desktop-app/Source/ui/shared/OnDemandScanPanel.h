// desktop-app/Source/ui/shared/OnDemandScanPanel.h
// Shared skeleton for the on-demand analysis cards added for the formerly
// orphan service routes (audit 2026-11-06 §9.2): one Run button, a status
// line, the standard error state, and one background fetch that follows the
// session-epoch contract (busy state reset first, stale results dropped).
// These cards are NOT auto-scans and never take part in the results gate.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../../IAppController.h"
#include "AnalysisFeatureText.h"
#include "KhensuErrorState.h"
#include "KhensuTheme.h" // ThemeListener
#include "PanelCard.h"
#include "PanelRunState.h" // UX-8 / ENG-3: status, error state, fetchThenPublish
#include <khensu/ipc/HttpServiceBridge.h>
#include <functional>
#include <utility>

namespace khensu {

class OnDemandScanPanel : public PanelCard {
  public:
    OnDemandScanPanel(IAppController& c, const juce::String& title, const juce::String& buttonText,
                      const juce::String& buttonTooltip)
        : PanelCard(c, title), app_(c)
    {
        runButton_.setButtonText(buttonText);
        runButton_.setTooltip(buttonTooltip);
        runButton_.setTitle(buttonText);
        runButton_.onClick = [this] { run(); };
        addAndMakeVisible(runButton_);
        run_.onRetry([this] { run(); });
        applyBaseThemeColours();
    }

    void resetForSession() override
    {
        clearResult();
        run_.reset(idleText());
        refreshAvailability();
        resized();
        repaint();
    }

    void resized() override
    {
        PanelCard::resized();
        auto b = bodyArea();
        auto controls = b.removeFromTop(24);
        runButton_.setBounds(controls.removeFromLeft(juce::jmin(130, controls.getWidth() / 2)));
        controls.removeFromLeft(8);
        statusLabel_.setBounds(controls);
        b.removeFromTop(4);
        errorState_.setBounds(b);
        layoutBody(b);
    }

    /// Re-evaluates whether Run is possible (e.g. stems landed). Default:
    /// enabled whenever no scan is in flight.
    virtual void refreshAvailability() { run_.setCanRun(canRun()); }

    bool isBusy() const noexcept { return run_.isBusy(); }

  protected:
    /// Starts the scan (button / retry). Derived classes validate and call
    /// startScan().
    virtual void run() = 0;
    /// Forget the shown result (session change).
    virtual void clearResult() = 0;
    /// Lay out derived children below the controls row.
    virtual void layoutBody(juce::Rectangle<int>) {}
    /// Text shown when nothing has run yet.
    virtual juce::String idleText() const { return {}; }
    /// Extra precondition for the Run button (default: always).
    virtual bool canRun() const { return true; }

    /// Runs `fetch` (worker thread; returns khensu::Result<T, BridgeError>)
    /// and hands a success to `publish` (message thread) - only when the
    /// panel still exists, the session is unchanged and no reset happened.
    template <typename Fetch, typename Publish>
    void startScan(const juce::String& busyText, Fetch fetch, Publish publish, bool longPool = false)
    {
        if (run_.isBusy()) return;
        FetchOptions options;
        options.longPool = longPool;
        options.onSettled = [this] { refreshAvailability(); };
        // `publish` sets its own status text; the empty return keeps it.
        fetchThenPublish(
            *this, app_, run_, busyText, std::move(fetch),
            [publish = std::move(publish)](OnDemandScanPanel&, const auto& value) mutable -> juce::String {
                publish(value);
                return {};
            },
            std::move(options));
    }

    /// Paint helper: true when the body should draw a result.
    bool showsResult() const noexcept { return run_.showsResult(); }

    /// Theme colour for a reading's tone (resolved live in paint()).
    static juce::Colour toneColour(feature_text::Tone tone)
    {
        switch (tone) {
        case feature_text::Tone::Good:
            return KhensuTheme::accentGreen;
        case feature_text::Tone::Info:
            return KhensuTheme::accentBlue;
        case feature_text::Tone::Moderate:
            return KhensuTheme::accentAmber;
        case feature_text::Tone::Severe:
            return KhensuTheme::accentRed;
        case feature_text::Tone::Neutral:
        default:
            return KhensuTheme::textSecondary;
        }
    }

    IAppController& app_;
    juce::TextButton runButton_;
    /// Status line + error state + busy state of the run button (UX-8).
    PanelRunState run_{this, &runButton_};
    juce::Label& statusLabel_ = run_.label();
    KhensuErrorState& errorState_ = run_.errorState();

  private:
    void applyBaseThemeColours()
    {
        runButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
        runButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    }

    // Declared last - see ThemeListener's note on member ordering. Derived
    // classes cache no colours of their own (they paint from the palette).
    ThemeListener baseThemeListener_{[this] {
        applyBaseThemeColours();
        repaint();
    }};
};

} // namespace khensu
