// plugin-copilot/Source/CopilotUndoHistory.cpp
#include "CopilotUndoHistory.h"

#include <cmath>

namespace khensu::copilot {

namespace {
bool onMessageThread() noexcept
{
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    return mm != nullptr && mm->isThisTheMessageThread();
}

constexpr float kSame = 1.0e-6f;
} // namespace

//==============================================================================
class ParamUndoHistory::ChangeAction final : public juce::UndoableAction {
  public:
    ChangeAction(ParamUndoHistory& owner, std::vector<Change> changes, std::function<void()> onUndo)
        : owner_(owner), changes_(std::move(changes)), onUndo_(std::move(onUndo))
    {
    }
    bool perform() override
    {
        // The first perform records what already happened; later ones redo.
        if (!first_) owner_.apply(changes_, true);
        first_ = false;
        return true;
    }
    bool undo() override
    {
        owner_.apply(changes_, false);
        if (onUndo_) onUndo_();
        return true;
    }
    int getSizeInUnits() override { return 10; }

  private:
    ParamUndoHistory& owner_;
    std::vector<Change> changes_;
    std::function<void()> onUndo_;
    bool first_ = true;
};

class ParamUndoHistory::CustomAction final : public juce::UndoableAction {
  public:
    CustomAction(ParamUndoHistory& owner, std::function<void()> perform, std::function<void()> undo)
        : owner_(owner), perform_(std::move(perform)), undo_(std::move(undo))
    {
    }
    bool perform() override
    {
        const ScopedSuppress quiet(owner_);
        if (perform_) perform_();
        return true;
    }
    bool undo() override
    {
        const ScopedSuppress quiet(owner_);
        if (undo_) undo_();
        return true;
    }
    int getSizeInUnits() override { return 10; }

  private:
    ParamUndoHistory& owner_;
    std::function<void()> perform_, undo_;
};

//==============================================================================
ParamUndoHistory::ScopedSuppress::ScopedSuppress(ParamUndoHistory& h) : history(h), active(onMessageThread())
{
    if (active) ++history.suppressDepth_;
}

ParamUndoHistory::ScopedSuppress::~ScopedSuppress()
{
    if (active) --history.suppressDepth_;
}

//==============================================================================
ParamUndoHistory::ParamUndoHistory(juce::AudioProcessor& processor) : manager_(2000, 20)
{
    // ~200 steps (10 units each); the history menu shows the last 20.
    parameters_ = processor.getParameters();
    for (auto* parameter : parameters_) parameter->addListener(this);
}

ParamUndoHistory::~ParamUndoHistory()
{
    for (auto* parameter : parameters_) parameter->removeListener(this);
}

void ParamUndoHistory::serviceClear()
{
    if (clearPending_.exchange(false, std::memory_order_acq_rel)) clear();
}

std::vector<float> ParamUndoHistory::capture() const
{
    std::vector<float> values;
    values.reserve(static_cast<size_t>(parameters_.size()));
    for (auto* parameter : parameters_) values.push_back(parameter->getValue());
    return values;
}

std::vector<ParamUndoHistory::Change> ParamUndoHistory::diff(const std::vector<float>& before) const
{
    std::vector<Change> changes;
    for (int i = 0; i < parameters_.size() && i < static_cast<int>(before.size()); ++i) {
        const float now = parameters_[i]->getValue();
        const float was = before[static_cast<size_t>(i)];
        if (std::abs(now - was) > kSame) changes.push_back({i, was, now});
    }
    return changes;
}

void ParamUndoHistory::apply(const std::vector<Change>& changes, bool forward)
{
    const ScopedSuppress quiet(*this);
    for (const auto& change : changes) {
        if (change.index < 0 || change.index >= parameters_.size()) continue;
        auto* parameter = parameters_[change.index];
        const float target = forward ? change.after : change.before;
        if (std::abs(parameter->getValue() - target) <= kSame) continue;
        parameter->beginChangeGesture(); // the host records it as an edit
        parameter->setValueNotifyingHost(target);
        parameter->endChangeGesture();
    }
}

void ParamUndoHistory::commit(const std::vector<float>& before, const juce::String& name, std::function<void()> onUndo)
{
    auto changes = diff(before);
    if (changes.empty()) return;
    const auto stepName = name.isNotEmpty() ? name : nameForGesture(changes);
    // JUCE's perform(action, name) only renames the open transaction; every
    // step is a transaction of its own.
    manager_.beginNewTransaction(stepName);
    manager_.perform(new ChangeAction(*this, std::move(changes), std::move(onUndo)));
    ++revision_;
}

void ParamUndoHistory::parameterGestureChanged(int parameterIndex, bool gestureIsStarting)
{
    // Hosts may call this on any thread; only the message thread records.
    if (!onMessageThread()) return;
    serviceClear();
    if (suppressDepth_ > 0 || transactionDepth_ > 0 || manager_.isPerformingUndoRedo()) return;
    if (gestureIsStarting) {
        if (gestureDepth_++ == 0) {
            gestureParameter_ = parameterIndex;
            gestureBefore_ = capture();
        }
        return;
    }
    if (gestureDepth_ == 0) return; // began while suppressed or inside a transaction
    if (--gestureDepth_ > 0) return;
    const auto before = std::move(gestureBefore_);
    gestureBefore_.clear();
    commit(before, {}, {});
}

void ParamUndoHistory::beginTransaction(const juce::String& name, std::function<void()> onUndo)
{
    jassert(onMessageThread());
    serviceClear();
    if (transactionDepth_++ > 0) return;
    transactionName_ = name;
    transactionOnUndo_ = std::move(onUndo);
    transactionBefore_ = capture();
}

void ParamUndoHistory::setTransactionName(const juce::String& name)
{
    if (transactionDepth_ > 0) transactionName_ = name;
}

void ParamUndoHistory::endTransaction()
{
    if (transactionDepth_ == 0) return;
    if (--transactionDepth_ > 0) return;
    const auto before = std::move(transactionBefore_);
    transactionBefore_.clear();
    auto onUndo = std::move(transactionOnUndo_);
    transactionOnUndo_ = nullptr;
    commit(before, transactionName_.isNotEmpty() ? transactionName_ : juce::String("Edit"), std::move(onUndo));
}

void ParamUndoHistory::performCustom(const juce::String& name, std::function<void()> perform,
                                     std::function<void()> undo)
{
    jassert(onMessageThread());
    serviceClear();
    if (manager_.isPerformingUndoRedo()) { // never nest a step inside an undo
        if (perform) perform();
        return;
    }
    manager_.beginNewTransaction(name);
    manager_.perform(new CustomAction(*this, std::move(perform), std::move(undo)));
    ++revision_;
}

bool ParamUndoHistory::canUndo() const { return !clearPending_.load(std::memory_order_acquire) && manager_.canUndo(); }
bool ParamUndoHistory::canRedo() const { return !clearPending_.load(std::memory_order_acquire) && manager_.canRedo(); }

bool ParamUndoHistory::undo()
{
    serviceClear();
    const bool done = manager_.undo();
    if (done) ++revision_;
    return done;
}

bool ParamUndoHistory::redo()
{
    serviceClear();
    const bool done = manager_.redo();
    if (done) ++revision_;
    return done;
}

int ParamUndoHistory::undoSteps(int steps)
{
    int done = 0;
    while (done < steps && undo()) ++done;
    return done;
}

int ParamUndoHistory::redoSteps(int steps)
{
    int done = 0;
    while (done < steps && redo()) ++done;
    return done;
}

juce::StringArray ParamUndoHistory::undoNames(int max) const
{
    if (clearPending_.load(std::memory_order_acquire)) return {};
    auto names = manager_.getUndoDescriptions(); // most recent first
    if (names.size() > max) names.removeRange(max, names.size() - max);
    return names;
}

juce::StringArray ParamUndoHistory::redoNames(int max) const
{
    if (clearPending_.load(std::memory_order_acquire)) return {};
    auto names = manager_.getRedoDescriptions(); // next redo first
    if (names.size() > max) names.removeRange(max, names.size() - max);
    return names;
}

void ParamUndoHistory::clear()
{
    manager_.clearUndoHistory();
    gestureDepth_ = 0;
    gestureBefore_.clear();
    ++revision_;
}

//==============================================================================
juce::String ParamUndoHistory::friendlyName(const juce::String& parameterName)
{
    juce::StringArray words;
    words.addTokens(parameterName.trim(), " ", "");
    words.removeEmptyStrings();
    juce::StringArray out;
    for (int i = 0; i < words.size(); ++i) {
        auto word = words[i];
        // "EQ2" -> "EQ 2"
        const int digits = word.length() - word.trimCharactersAtEnd("0123456789").length();
        juce::String number;
        if (digits > 0 && digits < word.length()) {
            number = word.getLastCharacters(digits);
            word = word.dropLastCharacters(digits);
        }
        const bool acronym = word.length() > 1 && word == word.toUpperCase();
        if (i > 0 && !acronym) word = word.toLowerCase();
        out.add(word);
        if (number.isNotEmpty()) out.add(number);
    }
    return out.joinIntoString(" ");
}

ui::KhensuKnob::Unit ParamUndoHistory::unitFor(const juce::String& paramId)
{
    using Unit = ui::KhensuKnob::Unit;
    if (paramId.endsWith("_db")) return Unit::Db;
    if (paramId.endsWith("_ms")) return Unit::Ms;
    if (paramId.contains("freq")) return Unit::Hz;
    if (paramId.contains("ratio")) return Unit::Ratio;
    if (paramId.endsWith("_q")) return Unit::Q;
    if (paramId == "chain_mix" || paramId == "fix_amount") return Unit::Percent;
    return Unit::None;
}

juce::String ParamUndoHistory::describe(const juce::RangedAudioParameter& parameter, float normalisedValue)
{
    auto name = friendlyName(parameter.getName(64));
    if (dynamic_cast<const juce::AudioParameterBool*>(&parameter) != nullptr) {
        if (name.endsWithIgnoreCase(" on")) name = name.dropLastCharacters(3);
        return name + (normalisedValue >= 0.5f ? " on" : " off");
    }
    if (auto* choice = dynamic_cast<const juce::AudioParameterChoice*>(&parameter)) {
        const int index = juce::jlimit(0, choice->choices.size() - 1,
                                       juce::roundToInt(normalisedValue * static_cast<float>(choice->choices.size() - 1)));
        return name + " " + choice->choices[index];
    }
    const double plain = static_cast<double>(parameter.convertFrom0to1(normalisedValue));
    return name + " " + ui::KhensuKnob::formatValue(plain, unitFor(parameter.getParameterID()));
}

juce::String ParamUndoHistory::nameForGesture(const std::vector<Change>& changes) const
{
    const auto describeChange = [this](const Change& change) -> juce::String {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameters_[change.index]))
            return describe(*ranged, change.after);
        return parameters_[change.index]->getName(64);
    };
    if (changes.size() == 1) return describeChange(changes.front());
    // The control the gesture started on names it; the rest follow it.
    const Change* lead = &changes.front();
    for (const auto& change : changes)
        if (change.index == gestureParameter_) lead = &change;
    return describeChange(*lead) + " (+" + juce::String(static_cast<int>(changes.size()) - 1) + ")";
}

} // namespace khensu::copilot
