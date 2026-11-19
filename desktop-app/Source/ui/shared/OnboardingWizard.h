// desktop-app/Source/ui/shared/OnboardingWizard.h
// First-run onboarding wizard: three steps, goal -> target -> suggested
// starting chain (see OnboardingPlan). Fully keyboard-operable: Tab moves
// between controls, arrow keys move within the goal choices, Return
// advances, Escape skips.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "ModalCardHost.h"
#include "KhensuTheme.h" // ThemeListener
#include "OnboardingPlan.h"
#include <khensu/targets/GenreDatabase.h>

namespace khensu {

class OnboardingWizard final : public ModalCardHost {
  public:
    explicit OnboardingWizard(const GenreDatabase& genres);
    ~OnboardingWizard() override;

    /// Finish: the chosen plan and whether to send its chain to the plugin.
    std::function<void(const OnboardingPlan&, bool pushToPlugin)> onFinish;
    std::function<void()> onSkip;

    void resized() override;
    int currentStep() const noexcept { return step_; }
    OnboardingPlan currentPlan() const;

    /// Test/automation hooks mirroring the UI controls.
    void chooseGoal(OnboardingPlan::Goal g);
    void chooseGenreIndex(int index);
    void choosePlatform(OnboardingPlan::Platform p);
    void next();
    void back();

  protected:
    juce::Rectangle<int> cardBounds() const override { return card_.getBounds(); }
    void dismiss() override
    {
        if (onSkip) onSkip();
    }

  private:
    struct Card final : public juce::Component {
        void paint(juce::Graphics& g) override;
    };

    /// Up/Down move between the goal choices (radio-group behaviour).
    struct GoalKeys final : public juce::KeyListener {
        explicit GoalKeys(OnboardingWizard& w) : wizard(w) {}
        bool keyPressed(const juce::KeyPress& key, juce::Component*) override;
        OnboardingWizard& wizard;
    };

    void showStep(int step);
    void refreshPlanText();

    const GenreDatabase& genres_;
    int step_ = 0;
    OnboardingPlan::Goal goal_ = OnboardingPlan::Goal::MixCheck;

    Card card_;
    juce::Label stepLabel_, headingLabel_, bodyLabel_;
    juce::OwnedArray<juce::ToggleButton> goalButtons_;
    juce::Label genreLabel_{{}, TRANS("Genre")};
    juce::ComboBox genreBox_;
    juce::Label platformLabel_{{}, TRANS("Where will it be heard?")};
    juce::ComboBox platformBox_;
    juce::Label targetLabel_;
    juce::TextEditor chainText_;
    juce::ToggleButton pushToggle_{TRANS("Send this starting chain to the Copilot plugin")};
    juce::TextButton skipButton_{TRANS("Skip")};
    juce::TextButton backButton_{TRANS("Back")};
    juce::TextButton nextButton_{TRANS("Next")};
    GoalKeys goalKeys_{*this};

    void applyThemeColours();
    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OnboardingWizard)
};

} // namespace khensu
