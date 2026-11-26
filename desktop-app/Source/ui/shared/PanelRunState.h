// desktop-app/Source/ui/shared/PanelRunState.h
// UX-8 / ENG-3: the run button + status line + error state every on-demand
// panel hand-rolled (about 108 status labels, 58 fetch paths), and the one
// fetch-then-publish path behind them.
//
//   PanelRunState run_{this, &analyzeButton_};          // member
//   run_.setIdle(TRANS("Runs at intake; re-run any time."));
//   fetchThenPublish(*this, controller_, run_, TRANS("Folding to mono..."),
//       [path] { return ipc::HttpServiceBridge{}.postAnalyzeJson(...); },
//       [](MonoCompatPanel& self, const juce::var& v) { ... });
//
// fetchThenPublish follows the session-epoch contract (CLAUDE.md): the fetch
// runs on the controller's short pool (or the long one), the result comes
// back on the message thread, the busy state is reset first - also for a
// stale result - a result from a superseded session is dropped, an error is
// classified once (ServiceErrorHint) and shown in the shared error state,
// and only a success reaches `publish`.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../../IBackgroundWork.h"
#include "KhensuErrorState.h"
#include "KhensuTheme.h"
#include "ServiceErrorHint.h"
#include "SessionEpoch.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <functional>
#include <type_traits>
#include <utility>

namespace khensu {

class PanelRunState {
  public:
    /// Adds the status label and the (hidden) error state to `owner`;
    /// `runButton` (may be null) is disabled while a run is in flight.
    PanelRunState(juce::Component* owner, juce::Button* runButton = nullptr) : runButton_(runButton)
    {
        status_.setFont(KhensuTheme::labelFont(type::caption));
        status_.setMinimumHorizontalScale(0.8f);
        applyThemeColours();
        if (owner != nullptr) {
            owner->addAndMakeVisible(status_);
            owner->addChildComponent(error_);
        }
    }

    void onRetry(std::function<void()> retry) { error_.onRetry(std::move(retry)); }

    void setIdle(const juce::String& text)
    {
        busy_ = false;
        status_.setText(text, juce::dontSendNotification);
        error_.setVisible(false);
        syncButton();
    }
    void setBusy(const juce::String& text)
    {
        busy_ = true;
        status_.setText(text, juce::dontSendNotification);
        syncButton();
    }
    /// The run ended (success or failure): the button comes back first, as
    /// the epoch contract requires, even when the result is then dropped.
    void settle()
    {
        busy_ = false;
        syncButton();
    }
    void setDone(const juce::String& text)
    {
        settle();
        hasResult_ = true;
        status_.setText(text, juce::dontSendNotification);
        error_.setVisible(false);
    }
    /// One wording for every service failure (ServiceErrorHint), shown in the
    /// error state; the status line says the run failed.
    void setError(const ipc::BridgeError& error, const juce::String& statusText = {})
    {
        settle();
        const auto hint = classifyServiceError(error);
        status_.setText(statusText.isNotEmpty() ? statusText : TRANS("Analysis failed."), juce::dontSendNotification);
        error_.setError(hint.message, hint.remedy);
        error_.setVisible(true);
    }
    /// Session change: no result, no error, idle text.
    void reset(const juce::String& idleText = {})
    {
        hasResult_ = false;
        ++generation_;
        setIdle(idleText);
    }

    /// The paint branch's test: a result is shown and no error is.
    bool showsResult() const noexcept { return hasResult_ && !error_.isVisible(); }
    bool isBusy() const noexcept { return busy_; }
    bool hasError() const noexcept { return error_.isVisible(); }
    int generation() const noexcept { return generation_; }
    /// Extra condition for the run button (e.g. stems present).
    void setCanRun(bool canRun)
    {
        canRun_ = canRun;
        syncButton();
    }

    juce::Label& label() noexcept { return status_; }
    KhensuErrorState& errorState() noexcept { return error_; }

    void applyThemeColours() { status_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary); }

  private:
    void syncButton()
    {
        if (runButton_ != nullptr) runButton_->setEnabled(!busy_ && canRun_);
    }

    juce::Button* runButton_ = nullptr;
    juce::Label status_;
    KhensuErrorState error_;
    bool busy_ = false, hasResult_ = false, canRun_ = true;
    int generation_ = 0;
    ThemeListener themeListener_{[this] { applyThemeColours(); }};
};

struct FetchOptions {
    bool longPool = false; ///< runLongInBackground (render / agent loops, chat)
    /// Runs on the message thread right after settle(), for every outcome -
    /// e.g. AutoScanCompletion::finish() - before the stale-session check.
    std::function<void()> onSettled;
    /// Status text on failure (default "Analysis failed.").
    juce::String failedText;
};

/// Runs `fetch` (worker thread; returns khensu::Result<T, ipc::BridgeError>)
/// and hands a success to `publish(Owner&, const T&)` on the message thread -
/// only while `owner` exists, its session is current and `run` was not reset
/// since. `publish` returns the done text (empty keeps the status line as is).
template <typename Owner, typename Fetch, typename Publish>
void fetchThenPublish(Owner& owner, IBackgroundWork& work, PanelRunState& run, const juce::String& busyText,
                      Fetch fetch, Publish publish, FetchOptions options = {})
{
    run.setBusy(busyText);
    juce::Component::SafePointer<Owner> self(&owner);
    PanelRunState* runPtr = &run;
    IBackgroundWork* workPtr = &work;
    const int generation = run.generation();
    auto job = [self, runPtr, workPtr, generation, fetch = std::move(fetch), publish = std::move(publish),
                options = std::move(options)]() mutable {
        auto result = fetch();
        juce::MessageManager::callAsync([self, runPtr, workPtr, generation, result = std::move(result),
                                         publish = std::move(publish), options = std::move(options),
                                         taskEpoch = session_epoch::current()]() mutable {
            if (self == nullptr) return; // the panel (and its PanelRunState) is gone
            runPtr->settle();            // busy state first - also on a stale result
            if (options.onSettled) options.onSettled();
            if (!session_epoch::stillCurrent(taskEpoch, *workPtr)) return; // superseded session
            if (generation != runPtr->generation()) return;                // reset since
            if (result.is_err()) {
                runPtr->setError(result.error(), options.failedText);
            } else {
                const juce::String done = publish(*self, result.value());
                runPtr->setDone(done.isNotEmpty() ? done : runPtr->label().getText());
            }
            self->resized();
            self->repaint();
        });
    };
    if (options.longPool)
        work.runLongInBackground(std::move(job));
    else
        work.runInBackground(std::move(job));
}

/// The session-epoch contract for a worker that posts back itself (UX-8):
/// chained fetches, poll loops, progress updates, a status line several
/// buttons share. Call it ON THE WORKER, once per message to post: it records
/// the current session, and on the message thread runs `settle(Owner&)`
/// (busy flags - also for a stale result), then `apply(Owner&)` only while
/// `owner` exists and the session is unchanged, or `onStale(Owner&)` when it
/// changed. Pass nullptr for a step you do not need.
template <typename Owner, typename Apply, typename Settle = std::nullptr_t, typename OnStale = std::nullptr_t>
void postIfCurrent(juce::Component::SafePointer<Owner> owner, IBackgroundWork& work, Apply apply,
                   Settle settle = nullptr, OnStale onStale = nullptr)
{
    IBackgroundWork* workPtr = &work;
    juce::MessageManager::callAsync([owner, workPtr, apply = std::move(apply), settle = std::move(settle),
                                     onStale = std::move(onStale), taskEpoch = session_epoch::current()]() mutable {
        auto* self = owner.getComponent();
        if (self == nullptr) return;
        if constexpr (!std::is_same_v<Settle, std::nullptr_t>) settle(*self);
        if (!session_epoch::stillCurrent(taskEpoch, *workPtr)) {
            if constexpr (!std::is_same_v<OnStale, std::nullptr_t>) onStale(*self);
            return;
        }
        apply(*self);
    });
}

/// The quiet path (UX-8): a best-effort fetch with no status line or run
/// button - a card filling itself in, a preview, a lookup. `fetch` runs on
/// the short pool (`longPool`: the long one). On the message thread,
/// `settle(Owner&)` runs first - busy flags, a "Rendering..." line - also for
/// a stale result; then `apply(Owner&, Result&&)` only while `owner` exists
/// and its session is still the current one (the session-epoch contract).
/// `apply` gets the whole result, error included. Pass nullptr for no settle.
template <typename Owner, typename Fetch, typename Apply, typename Settle = std::nullptr_t>
void fetchQuietly(Owner& owner, IBackgroundWork& work, Fetch fetch, Apply apply, Settle settle = nullptr,
                  bool longPool = false)
{
    juce::Component::SafePointer<Owner> self(&owner);
    IBackgroundWork* workPtr = &work;
    auto job = [self, workPtr, fetch = std::move(fetch), apply = std::move(apply),
                settle = std::move(settle)]() mutable {
        auto result = fetch();
        juce::MessageManager::callAsync([self, workPtr, result = std::move(result), apply = std::move(apply),
                                         settle = std::move(settle), taskEpoch = session_epoch::current()]() mutable {
            if (self == nullptr) return;
            if constexpr (!std::is_same_v<Settle, std::nullptr_t>) settle(*self);
            if (!session_epoch::stillCurrent(taskEpoch, *workPtr)) return; // superseded session
            apply(*self, std::move(result));
        });
    };
    if (longPool)
        work.runLongInBackground(std::move(job));
    else
        work.runInBackground(std::move(job));
}

} // namespace khensu
