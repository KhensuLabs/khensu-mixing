// desktop-app/Source/ui/MixMasterEngine/SuggestionListPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/ListenerHandle.h"
#include "../shared/BadgeComponent.h"
#include <khensu/analysis/AnalysisResult.h>
#include <memory>
#include <vector>
#include <string>
#include <map>

namespace khensu {

// THREAD_CONTEXT(UIThread)
class SuggestionListPanel final : public PanelCard {
  public:
    explicit SuggestionListPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void resized() override;

    /// Keyboard-driven "next issue": scrolls to and briefly highlights the
    /// next unresolved recommendation in severity order (wraps around).
    /// Returns false if there's nothing unresolved to jump to.
    bool focusNextIssue();

  private:
    // Painted header separating priority tiers (PRIORITY / RECOMMENDED / POLISH)
    struct GroupHeader final : public juce::Component {
        GroupHeader(const juce::String& text, juce::Colour col);
        void paint(juce::Graphics& g) override;
        juce::String text_;
        juce::Colour colour_;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GroupHeader)
    };

    struct RecommendationRow final : public juce::Component {
        RecommendationRow();
        void setData(const Recommendation& rec, bool isActedOn, bool isFixFirst);
        void paint(juce::Graphics& g) override;
        void resized() override;
        void mouseUp(const juce::MouseEvent& e) override
        {
            if (onRowClicked && e.mouseWasClicked()) onRowClicked();
        }
        std::string recId_;

        /// Brief highlight pulse used by the keyboard "next issue" command.
        void setHighlighted(bool h)
        {
            highlighted_ = h;
            repaint();
        }
        bool highlighted_ = false;

        BadgeComponent severityBadge;
        juce::Label typeLabel;
        juce::Label whatLabel;
        juce::Label actionLabel;
        juce::Label contextLabel;
        juce::Label conflictLabel;
        juce::Label fixFirstLabel;
        juce::TextButton resolveButton;
        juce::TextButton applyButton{TRANS("Fix This")};
        std::function<void()> onResolvePressed;
        std::function<void()> onApplyPressed; ///< one-click render via AppController::applyRecommendationChain
        std::function<void()> onRowClicked;   ///< click -> copilot explanation
        bool isActedOn_ = false;
        bool isFixFirst_ = false;
        bool hasParams_ = false;

        /// Transient visual feedback for the apply button - doesn't persist
        /// across the next setData() rebuild (recommendations refresh wholesale
        /// after an apply anyway, since the new render changes the analysis).
        void setApplyInFlight(bool inFlight)
        {
            applyButton.setEnabled(!inFlight);
            applyButton.setButtonText(inFlight ? "Applying..." : "Fix This");
        }
        void setApplyResult(bool success)
        {
            applyButton.setButtonText(success ? juce::String(juce::CharPointer_UTF8("Applied +"))
                                              : juce::String("Failed"));
            applyButton.setEnabled(false);
        }
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RecommendationRow)
    };

    juce::Viewport viewport_;
    juce::Component listContent_;
    std::vector<std::unique_ptr<RecommendationRow>> rows_;
    std::vector<std::unique_ptr<GroupHeader>> headers_;
    juce::Label emptyLabel_;

    std::vector<Recommendation> lastRecs_; // cached for focusNextIssue() cycling
    int nextIssueIndex_ = -1;

    static constexpr int kRowH = 72;
    static constexpr int kGroupHeaderH = 22;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // ENG-2: the issue-queue registration, deregistered before the members it touches.
    ListenerHandle issuesListener_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SuggestionListPanel)
};

} // namespace khensu
