// plugin-copilot/Source/CopilotUndoHistory.h
// UX2-6b: undo / redo for the Copilot's parameters, independent of the host.
//
// What is a step:
//   * a user gesture on a control (a knob drag, a wheel notch, a typed value,
//     a toggle, an EQ-curve band drag): everything that changed between the
//     first beginChangeGesture and the last endChangeGesture is ONE step,
//     named after the control ("EQ 2 gain -3.0 dB");
//   * a transaction the code opens around a compound change (preset load,
//     Apply top fix, a generated chain): one step whatever it touches;
//   * a custom step with its own do / undo (an A/B snapshot recall).
// What is not: host automation and state restores (they never call the
// plugin's gesture methods), and programmatic writes made under
// ScopedSuppress (the live fix re-applying, fix-automation writes).
//
// THREAD_CONTEXT(UIThread) for everything except the parameter listener,
// which hosts may call on any thread: off the message thread it returns at
// once (no locks, no allocation), so the audio thread is never touched.
// Undo / redo write the values back as host gestures.
#pragma once
#include <JuceHeader.h>
#include <khensu/ui/KhensuKnob.h>

#include <atomic>
#include <functional>
#include <vector>

namespace khensu::copilot {

class ParamUndoHistory final : private juce::AudioProcessorParameter::Listener {
  public:
    /// Tracks every parameter of `processor` (its parameters must outlive this).
    explicit ParamUndoHistory(juce::AudioProcessor& processor);
    ~ParamUndoHistory() override;

    /// Opens / closes a named compound change; nestable (the outermost name
    /// wins). Closing commits one step when anything changed. `onUndo` (if
    /// set) runs after the step is undone, e.g. to drop the live fix.
    void beginTransaction(const juce::String& name, std::function<void()> onUndo = {});
    void endTransaction();
    bool inTransaction() const noexcept { return transactionDepth_ > 0; }
    /// Renames the open transaction (e.g. once the applied amount is known).
    void setTransactionName(const juce::String& name);

    struct ScopedTransaction {
        ScopedTransaction(ParamUndoHistory& h, const juce::String& name, std::function<void()> onUndo = {})
            : history(h)
        {
            history.beginTransaction(name, std::move(onUndo));
        }
        ~ScopedTransaction() { history.endTransaction(); }
        ParamUndoHistory& history;
        JUCE_DECLARE_NON_COPYABLE(ScopedTransaction)
    };

    /// Parameter writes made while one is alive are never recorded on their
    /// own (they still show in an enclosing gesture's or transaction's diff).
    struct ScopedSuppress {
        explicit ScopedSuppress(ParamUndoHistory& h);
        ~ScopedSuppress();
        ParamUndoHistory& history;
        bool active;
        JUCE_DECLARE_NON_COPYABLE(ScopedSuppress)
    };

    /// Runs `perform` now and records it as step `name`; undo runs `undo`.
    /// Parameter writes inside either are not recorded separately.
    void performCustom(const juce::String& name, std::function<void()> perform, std::function<void()> undo);

    bool canUndo() const;
    bool canRedo() const;
    bool undo();
    bool redo();
    /// Undoes `steps` steps (the history menu jumping back); returns how many.
    int undoSteps(int steps);
    int redoSteps(int steps);
    /// Step names, most recent first, at most `max`.
    juce::StringArray undoNames(int max = 20) const;
    juce::StringArray redoNames(int max = 20) const;
    void clear();
    /// Any thread (a host restoring state): the history is cleared the next
    /// time the message thread touches it.
    void requestClear() noexcept { clearPending_.store(true, std::memory_order_release); }
    /// Carries out a pending requestClear() (the editor's timer calls it).
    void serviceRequests() { serviceClear(); }

    /// "EQ 2 gain -3.0 dB", "HPF on", "EQ 2 shape Low shelf".
    static juce::String describe(const juce::RangedAudioParameter& parameter, float normalisedValue);
    /// The unit a parameter id reads in ("eq_freq" -> Hz, "_db" -> dB, ...).
    static ui::KhensuKnob::Unit unitFor(const juce::String& paramId);
    /// "EQ2 Gain" -> "EQ 2 gain", "Comp Threshold" -> "Comp threshold".
    static juce::String friendlyName(const juce::String& parameterName);

    /// Bumped on every change to the history (the editor refreshes on it).
    int revision() const noexcept { return revision_; }

  private:
    struct Change {
        int index;
        float before, after; ///< normalised
    };
    class ChangeAction;
    class CustomAction;

    void parameterValueChanged(int, float) override {}
    void parameterGestureChanged(int parameterIndex, bool gestureIsStarting) override;

    std::vector<float> capture() const;
    std::vector<Change> diff(const std::vector<float>& before) const;
    void apply(const std::vector<Change>& changes, bool forward);
    void commit(const std::vector<float>& before, const juce::String& name, std::function<void()> onUndo);
    juce::String nameForGesture(const std::vector<Change>& changes) const;
    void serviceClear();

    juce::UndoManager manager_;
    juce::Array<juce::AudioProcessorParameter*> parameters_;

    int gestureDepth_ = 0;
    int gestureParameter_ = -1;
    std::vector<float> gestureBefore_;

    int transactionDepth_ = 0;
    juce::String transactionName_;
    std::function<void()> transactionOnUndo_;
    std::vector<float> transactionBefore_;

    int suppressDepth_ = 0;
    int revision_ = 0;
    std::atomic<bool> clearPending_{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParamUndoHistory)
};

} // namespace khensu::copilot
