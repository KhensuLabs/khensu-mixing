// desktop-app/Source/ui/shared/OnboardingWizard.cpp
#include "OnboardingWizard.h"
#include "KhensuTheme.h"

namespace khensu {

namespace {
constexpr int kGoalRadioGroup = 4401;
const OnboardingPlan::Goal kGoals[] = {OnboardingPlan::Goal::MixCheck, OnboardingPlan::Goal::MasterForRelease,
                                       OnboardingPlan::Goal::VocalPolish, OnboardingPlan::Goal::MatchReference};
const char* kGoalBlurbs[] = {"Find what is holding the mix back before you master.",
                             "Hit streaming/club loudness with clean peaks.", "Level, de-harsh and seat the vocal.",
                             "Get the tone and loudness of a reference track."};
const OnboardingPlan::Platform kPlatforms[] = {OnboardingPlan::Platform::Streaming,
                                               OnboardingPlan::Platform::AppleMusic, OnboardingPlan::Platform::YouTube,
                                               OnboardingPlan::Platform::Club, OnboardingPlan::Platform::Broadcast};
} // namespace

void OnboardingWizard::Card::paint(juce::Graphics& g)
{
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 10.0f);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 10.0f, 1.0f);
}

OnboardingWizard::OnboardingWizard(const GenreDatabase& genres) : genres_(genres)
{
    drawCardBackground_ = false; // Card paints itself
    setTitle(TRANS("Welcome to Khensu - setup"));
    addAndMakeVisible(card_);
    card_.setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);

    stepLabel_.setFont(KhensuTheme::labelFont(11.0f));
    headingLabel_.setFont(KhensuTheme::uiFont(KhensuTheme::kFontH1, juce::Font::bold));
    bodyLabel_.setFont(KhensuTheme::bodyFont());
    for (auto* l : {&stepLabel_, &headingLabel_, &bodyLabel_})
        card_.addAndMakeVisible(l);

    int order = 1;
    for (size_t i = 0; i < std::size(kGoals); ++i) {
        auto* b =
            goalButtons_.add(new juce::ToggleButton(OnboardingPlan::goalName(kGoals[i]) + " - " + kGoalBlurbs[i]));
        b->setRadioGroupId(kGoalRadioGroup);
        b->setTitle(OnboardingPlan::goalName(kGoals[i]));
        b->setDescription(kGoalBlurbs[i]);
        b->setExplicitFocusOrder(order++);
        const auto goal = kGoals[i];
        b->onClick = [this, goal] { goal_ = goal; };
        card_.addChildComponent(b);
    }
    goalButtons_[0]->setToggleState(true, juce::dontSendNotification);

    int id = 1;
    for (const auto& g : genres_.listAll())
        genreBox_.addItem(juce::String(g.name), id++);
    if (genreBox_.getNumItems() == 0) genreBox_.addItem(TRANS("Pop"), 1);
    genreBox_.setSelectedItemIndex(0, juce::dontSendNotification);
    for (const auto& g : genres_.listAll())
        if (juce::String(g.id).equalsIgnoreCase("pop"))
            genreBox_.setText(juce::String(g.name), juce::dontSendNotification);
    genreBox_.setTitle(TRANS("Genre"));
    genreBox_.setTooltip(TRANS("Sets loudness, tonal-balance and dynamics targets."));
    genreBox_.onChange = [this] { refreshPlanText(); };
    platformBox_.setTitle(TRANS("Delivery platform"));
    platformBox_.setTooltip(TRANS("Each platform normalizes to a different loudness."));
    for (size_t i = 0; i < std::size(kPlatforms); ++i)
        platformBox_.addItem(OnboardingPlan::platformName(kPlatforms[i]), static_cast<int>(i) + 1);
    platformBox_.setSelectedId(1, juce::dontSendNotification);
    platformBox_.onChange = [this] { refreshPlanText(); };
    targetLabel_.setFont(KhensuTheme::bodyFont());
    for (juce::Component* c : std::initializer_list<juce::Component*>{&genreLabel_, &genreBox_, &platformLabel_,
                                                                      &platformBox_, &targetLabel_})
        card_.addChildComponent(c);

    chainText_.setMultiLine(true);
    chainText_.setReadOnly(true);
    chainText_.setTitle(TRANS("Suggested starting chain"));
    chainText_.setFont(KhensuTheme::bodyFont());
    card_.addChildComponent(chainText_);
    pushToggle_.setToggleState(true, juce::dontSendNotification);
    pushToggle_.setTooltip(TRANS("Pre-loads these values into the Khensu Copilot plugin's built-in DSP chain."));
    card_.addChildComponent(pushToggle_);

    skipButton_.setTooltip(TRANS("Close setup (Escape). You can reopen it from the command palette (Ctrl+K)."));
    skipButton_.onClick = [this] { dismiss(); };
    backButton_.onClick = [this] { back(); };
    nextButton_.onClick = [this] { next(); };
    for (auto* b : {&skipButton_, &backButton_, &nextButton_})
        card_.addAndMakeVisible(b);
    nextButton_.addShortcut(juce::KeyPress(juce::KeyPress::returnKey));

    card_.addKeyListener(&goalKeys_);
    applyThemeColours();
    showStep(0);
    grabModalFocus();
}

bool OnboardingWizard::GoalKeys::keyPressed(const juce::KeyPress& key, juce::Component*)
{
    if (wizard.step_ != 0) return false;
    const int dir = key == juce::KeyPress::downKey ? 1 : key == juce::KeyPress::upKey ? -1 : 0;
    if (dir == 0) return false;
    int current = 0;
    for (int i = 0; i < wizard.goalButtons_.size(); ++i)
        if (wizard.goalButtons_[i]->getToggleState()) current = i;
    const int nextIndex = juce::jlimit(0, wizard.goalButtons_.size() - 1, current + dir);
    wizard.chooseGoal(kGoals[nextIndex]);
    if (wizard.goalButtons_[nextIndex]->isShowing()) wizard.goalButtons_[nextIndex]->grabKeyboardFocus();
    return true;
}

OnboardingWizard::~OnboardingWizard() { card_.removeKeyListener(&goalKeys_); }

void OnboardingWizard::applyThemeColours()
{
    stepLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
    headingLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    bodyLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    genreLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    platformLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    targetLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
    for (auto* b : goalButtons_) {
        b->setColour(juce::ToggleButton::textColourId, KhensuTheme::textPrimary);
        b->setColour(juce::ToggleButton::tickColourId, KhensuTheme::accentTeal);
    }
    pushToggle_.setColour(juce::ToggleButton::textColourId, KhensuTheme::textPrimary);
    chainText_.setColour(juce::TextEditor::backgroundColourId, KhensuTheme::bgRaised);
    chainText_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
    chainText_.setColour(juce::TextEditor::outlineColourId, KhensuTheme::borderSubtle);
    for (auto* b : {&skipButton_, &backButton_, &nextButton_}) {
        b->setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.15f));
        b->setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    }
}

OnboardingPlan OnboardingWizard::currentPlan() const
{
    GenreEntry genre;
    const auto& all = genres_.listAll();
    const int gi = genreBox_.getSelectedItemIndex();
    if (gi >= 0 && gi < static_cast<int>(all.size())) genre = all[static_cast<size_t>(gi)];
    if (genre.id.empty()) {
        genre.id = "pop";
        genre.name = "Pop";
    }
    const int pi = juce::jlimit(0, static_cast<int>(std::size(kPlatforms)) - 1, platformBox_.getSelectedItemIndex());
    return OnboardingPlan::build(goal_, genre, kPlatforms[pi]);
}

void OnboardingWizard::chooseGoal(OnboardingPlan::Goal g)
{
    goal_ = g;
    for (size_t i = 0; i < std::size(kGoals); ++i)
        goalButtons_[static_cast<int>(i)]->setToggleState(kGoals[i] == g, juce::dontSendNotification);
}

void OnboardingWizard::chooseGenreIndex(int index)
{
    genreBox_.setSelectedItemIndex(index, juce::sendNotificationSync);
}

void OnboardingWizard::choosePlatform(OnboardingPlan::Platform p)
{
    for (size_t i = 0; i < std::size(kPlatforms); ++i)
        if (kPlatforms[i] == p) platformBox_.setSelectedItemIndex(static_cast<int>(i), juce::sendNotificationSync);
}

void OnboardingWizard::next()
{
    if (step_ < 2) {
        showStep(step_ + 1);
        return;
    }
    if (onFinish) onFinish(currentPlan(), pushToggle_.getToggleState());
}

void OnboardingWizard::back()
{
    if (step_ > 0) showStep(step_ - 1);
}

void OnboardingWizard::refreshPlanText()
{
    const auto plan = currentPlan();
    targetLabel_.setText(TRANS("Target: ") + juce::String(plan.targetLufs, 1) +
                             " LUFS integrated, peaks <= " + juce::String(plan.truePeakCeiling, 1) + " dBTP",
                         juce::dontSendNotification);
    juce::String text;
    int n = 1;
    for (const auto& s : plan.chain)
        text << n++ << ". " << s.name << "\n    " << s.why << "\n\n";
    chainText_.setText(text.trimEnd(), juce::dontSendNotification);
    bool anyParams = false;
    for (const auto& s : plan.chain)
        anyParams = anyParams || !s.params.empty();
    pushToggle_.setEnabled(anyParams);
}

void OnboardingWizard::showStep(int step)
{
    step_ = juce::jlimit(0, 2, step);
    stepLabel_.setText(TRANS("STEP ") + juce::String(step_ + 1) + " OF 3", juce::dontSendNotification);
    static const char* headings[] = {"What do you want to do?", "What are you aiming for?",
                                     "Your suggested starting chain"};
    static const char* bodies[] = {"Pick a goal - the rest of setup adapts to it.",
                                   "Your genre and platform set loudness, tone and dynamics targets.",
                                   "A starting point, not a preset to keep - every stage explains why."};
    headingLabel_.setText(headings[step_], juce::dontSendNotification);
    bodyLabel_.setText(bodies[step_], juce::dontSendNotification);

    for (auto* b : goalButtons_)
        b->setVisible(step_ == 0);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&genreLabel_, &genreBox_, &platformLabel_,
                                                                      &platformBox_, &targetLabel_})
        c->setVisible(step_ == 1);
    chainText_.setVisible(step_ == 2);
    pushToggle_.setVisible(step_ == 2);
    backButton_.setEnabled(step_ > 0);
    nextButton_.setButtonText(step_ == 2 ? TRANS("Finish") : TRANS("Next"));
    refreshPlanText();
    resized();

    // Move keyboard focus to the step's first control.
    juce::Component* first = step_ == 0   ? static_cast<juce::Component*>(goalButtons_[0])
                             : step_ == 1 ? static_cast<juce::Component*>(&genreBox_)
                                          : static_cast<juce::Component*>(&nextButton_);
    if (first != nullptr && first->isShowing()) first->grabKeyboardFocus();
}

void OnboardingWizard::resized()
{
    auto bounds = getLocalBounds();
    card_.setBounds(bounds.withSizeKeepingCentre(juce::jmin(620, bounds.getWidth() - 40),
                                                 juce::jmin(440, bounds.getHeight() - 40)));
    auto b = card_.getLocalBounds().reduced(24);
    stepLabel_.setBounds(b.removeFromTop(16));
    headingLabel_.setBounds(b.removeFromTop(30));
    bodyLabel_.setBounds(b.removeFromTop(22));
    b.removeFromTop(10);
    auto buttons = b.removeFromBottom(30);
    skipButton_.setBounds(buttons.removeFromLeft(80));
    nextButton_.setBounds(buttons.removeFromRight(100));
    buttons.removeFromRight(8);
    backButton_.setBounds(buttons.removeFromRight(80));
    b.removeFromBottom(10);

    if (step_ == 0) {
        for (auto* g : goalButtons_)
            g->setBounds(b.removeFromTop(36));
    } else if (step_ == 1) {
        genreLabel_.setBounds(b.removeFromTop(18));
        genreBox_.setBounds(b.removeFromTop(28).removeFromLeft(320));
        b.removeFromTop(12);
        platformLabel_.setBounds(b.removeFromTop(18));
        platformBox_.setBounds(b.removeFromTop(28).removeFromLeft(320));
        b.removeFromTop(16);
        targetLabel_.setBounds(b.removeFromTop(24));
    } else {
        pushToggle_.setBounds(b.removeFromBottom(26));
        b.removeFromBottom(6);
        chainText_.setBounds(b);
    }
}

} // namespace khensu
