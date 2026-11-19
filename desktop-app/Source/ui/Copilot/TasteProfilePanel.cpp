// desktop-app/Source/ui/Copilot/TasteProfilePanel.cpp
#include "TasteProfilePanel.h"
#include "../../IAppController.h"

namespace khensu {

namespace {

juce::String evidenceLine(const LearnedPreference& p)
{
    juce::StringArray parts;
    if (p.rejects > 0) parts.add(juce::String(p.rejects) + " " + (p.rejects == 1 ? TRANS("reject") : TRANS("rejects")));
    if (p.accepts > 0) parts.add(juce::String(p.accepts) + " " + (p.accepts == 1 ? TRANS("accept") : TRANS("accepts")));
    if (p.ab_picks > 0)
        parts.add(juce::String(p.ab_picks) + " " + (p.ab_picks == 1 ? TRANS("A/B pick") : TRANS("A/B picks")));
    juce::String line = parts.isEmpty() ? TRANS("No recorded history") : TRANS("From") + " " + parts.joinIntoString(", ");
    if (!p.last_updated.empty()) {
        const auto t = juce::Time::fromISO8601(juce::String(p.last_updated));
        if (t.toMilliseconds() != 0) line += " - " + TRANS("last changed") + " " + t.formatted("%d %b %Y");
    }
    return line;
}

} // namespace

// ---------------------------------------------------------------------------
// One learned preference: title, offset, explanation, evidence, Reset.
// ---------------------------------------------------------------------------
class TasteProfilePanel::Row final : public juce::Component {
  public:
    Row(const LearnedPreference& p, std::function<void()> onReset)
    {
        applyThemeColours();
        title_.setFont(KhensuTheme::uiFont(14.0f, juce::Font::bold));
        title_.setText(juce::String(p.title), juce::dontSendNotification);
        value_.setFont(KhensuTheme::metricFont(13.0f));
        value_.setJustificationType(juce::Justification::centredRight);
        value_.setText(juce::String(p.offset_text), juce::dontSendNotification);
        explanation_.setFont(KhensuTheme::bodyFont(13.0f));
        explanation_.setJustificationType(juce::Justification::topLeft);
        explanation_.setText(juce::String(p.explanation), juce::dontSendNotification);
        evidence_.setFont(KhensuTheme::labelFont(12.0f));
        evidence_.setText(evidenceLine(p), juce::dontSendNotification);

        reset_.setButtonText(TRANS("Reset"));
        reset_.setTitle(TRANS("Reset") + " " + juce::String(p.title));
        reset_.setTooltip(TRANS("Forget this learned preference and go back to the genre default"));
        reset_.onClick = std::move(onReset);

        for (auto* c : std::initializer_list<juce::Component*>{&title_, &value_, &explanation_, &evidence_, &reset_})
            addAndMakeVisible(c);
        setTitle(juce::String(p.title));
        setDescription(juce::String(p.explanation));
    }

    static constexpr int kHeight = 96;

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced(1.0f);
        g.setColour(KhensuTheme::bgRaised);
        g.fillRoundedRectangle(r, 6.0f);
        g.setColour(KhensuTheme::borderSubtle);
        g.drawRoundedRectangle(r, 6.0f, 1.0f);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced(KhensuTheme::sp3, KhensuTheme::sp2);
        auto top = b.removeFromTop(22);
        reset_.setBounds(top.removeFromRight(80).reduced(0, 1));
        top.removeFromRight(KhensuTheme::sp2);
        value_.setBounds(top.removeFromRight(juce::jmin(200, top.getWidth() / 2)));
        title_.setBounds(top);
        evidence_.setBounds(b.removeFromBottom(18));
        explanation_.setBounds(b);
    }

  private:
    void applyThemeColours()
    {
        title_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
        value_.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
        explanation_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
        evidence_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    }

    juce::Label title_, value_, explanation_, evidence_;
    juce::TextButton reset_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Row)
};

// ---------------------------------------------------------------------------

// Every cached palette colour on this panel, in one place (UI-01).
void TasteProfilePanel::applyThemeColours()
{
    headerLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
    introLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    emptyLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

TasteProfilePanel::TasteProfilePanel(IAppController& c) : controller_(c)
{
    applyThemeColours();
    headerLabel_.setFont(KhensuTheme::uiFont(15.0f, juce::Font::bold));
    headerLabel_.setText(TRANS("What Khensu Has Learned About Your Taste"), juce::dontSendNotification);
    addAndMakeVisible(headerLabel_);

    introLabel_.setFont(KhensuTheme::bodyFont(13.0f));
    introLabel_.setJustificationType(juce::Justification::topLeft);
    introLabel_.setText(TRANS("These offsets come from the recommendations you accept or reject and the side you "
                              "pick in A/B comparisons. They shift the genre targets that recommendations, the "
                              "mixer chain generator and the mastering agent measure against."),
                        juce::dontSendNotification);
    addAndMakeVisible(introLabel_);

    emptyLabel_.setFont(KhensuTheme::bodyFont(13.0f));
    emptyLabel_.setJustificationType(juce::Justification::topLeft);
    emptyLabel_.setText(TRANS("Nothing learned yet. Accept or reject loudness, dynamics, stereo-width or low-end "
                              "recommendations, or pick a side with the ABX overlay's \"I Prefer\" buttons, and "
                              "what Khensu learns will show up here."),
                        juce::dontSendNotification);
    addChildComponent(emptyLabel_);

    resetAllButton_.setButtonText(TRANS("Reset all"));
    resetAllButton_.setTitle(TRANS("Reset all learned preferences"));
    resetAllButton_.setTooltip(TRANS("Forget everything Khensu has learned about your taste"));
    resetAllButton_.onClick = [this] { confirmResetAll(); };
    addAndMakeVisible(resetAllButton_);

    viewport_.setViewedComponent(&rowsHolder_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);

    show({});
}

TasteProfilePanel::~TasteProfilePanel() = default;

void TasteProfilePanel::visibilityChanged()
{
    if (isVisible()) refresh();
}

void TasteProfilePanel::refresh()
{
    juce::Component::SafePointer<TasteProfilePanel> self(this);
    auto* fm = controller_.analysis().getFeedbackManager();
    if (fm == nullptr) {
        show({});
        return;
    }
    // Learned taste is global (not session-scoped), so no session-epoch check.
    controller_.runInBackground([self, fm] {
        auto prefs = learned_preferences::describe(*fm);
        juce::MessageManager::callAsync([self, prefs = std::move(prefs)]() mutable {
            if (self != nullptr) self->show(std::move(prefs));
        });
    });
}

void TasteProfilePanel::show(std::vector<LearnedPreference> prefs)
{
    shown_ = std::move(prefs);
    rows_.clear();
    for (const auto& p : shown_) {
        const std::string key = p.pref_key;
        rows_.push_back(std::make_unique<Row>(p, [this, key] { resetOne(key); }));
        rowsHolder_.addAndMakeVisible(*rows_.back());
    }
    emptyLabel_.setVisible(shown_.empty());
    resetAllButton_.setEnabled(!shown_.empty());
    resized();
}

void TasteProfilePanel::resetOne(const std::string& prefKey)
{
    juce::Component::SafePointer<TasteProfilePanel> self(this);
    auto* fm = controller_.analysis().getFeedbackManager();
    if (fm == nullptr) return;
    controller_.runInBackground([self, fm, prefKey] {
        learned_preferences::reset(*fm, prefKey);
        juce::MessageManager::callAsync([self] {
            if (self != nullptr) self->refresh();
        });
    });
}

void TasteProfilePanel::confirmResetAll()
{
    juce::Component::SafePointer<TasteProfilePanel> self(this);
    juce::AlertWindow::showOkCancelBox(
        juce::AlertWindow::WarningIcon, TRANS("Reset everything Khensu has learned?"),
        TRANS("Loudness, dynamics, stereo-width and low-end preferences go back to the genre defaults and their "
              "history is cleared. Your sessions, settings and feedback notes are kept. This cannot be undone."),
        TRANS("Reset all"), TRANS("Cancel"), this, juce::ModalCallbackFunction::create([self](int result) {
            if (result == 1 && self != nullptr) self->resetAll();
        }));
}

void TasteProfilePanel::resetAll()
{
    juce::Component::SafePointer<TasteProfilePanel> self(this);
    auto* fm = controller_.analysis().getFeedbackManager();
    if (fm == nullptr) return;
    controller_.runInBackground([self, fm] {
        learned_preferences::resetAll(*fm);
        juce::MessageManager::callAsync([self] {
            if (self != nullptr) self->refresh();
        });
    });
}

void TasteProfilePanel::paint(juce::Graphics& g) { g.fillAll(KhensuTheme::bgBase); }

void TasteProfilePanel::layoutRows()
{
    const int w = juce::jmax(0, viewport_.getMaximumVisibleWidth());
    int y = 0;
    for (auto& row : rows_) {
        row->setBounds(0, y, w, Row::kHeight);
        y += Row::kHeight + KhensuTheme::sp2;
    }
    rowsHolder_.setSize(w, juce::jmax(0, y));
}

void TasteProfilePanel::resized()
{
    auto b = getLocalBounds().reduced(KhensuTheme::sp3);
    auto header = b.removeFromTop(32);
    resetAllButton_.setBounds(header.removeFromRight(100).reduced(0, 3));
    headerLabel_.setBounds(header);
    introLabel_.setBounds(b.removeFromTop(48));
    b.removeFromTop(KhensuTheme::sp2);
    if (emptyLabel_.isVisible()) emptyLabel_.setBounds(b.removeFromTop(60));
    viewport_.setBounds(b);
    layoutRows();
}

} // namespace khensu
