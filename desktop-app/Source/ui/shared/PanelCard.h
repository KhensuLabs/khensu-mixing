// desktop-app/Source/ui/shared/PanelCard.h
#pragma once
#include <JuceHeader.h>
#include <vector>
#include <string>
#include <memory>
#include "KhensuSkeleton.h"
#include "ExperimentalBadge.h"
#include "../../IAppController.h"

namespace khensu {

/// ENG-1: a panel card holds the IAppController hub; it reaches a service
/// through it (`controller_.playback()`), never through AppController.
using PanelCardController = IAppController;

enum class PanelAnalysisState { NotApplicable, NotAnalyzed, Analyzing, Analyzed };

class CardFindingRow;

class PanelCard : public juce::Component {
  public:
    PanelCard(PanelCardController& controller, const juce::String& title);
    ~PanelCard() override;

    /// Session change (S4 session-reset fan-out): forget every result that
    /// belongs to the previous session - scan rows, loaded references, an
    /// auto-run queued or pending. The owning screen (a SessionScoped
    /// registered with AppController) calls it on every session change, on
    /// the message thread, before the next session's analysis starts. The
    /// default does nothing (panels fed only by setData() are reset by the
    /// owner pushing an empty result).
    virtual void resetForSession() {}

    void setExplainer(const juce::String& category, const juce::String& description,
                      const juce::String& systemBrainFeatureId = {});

    void setAnalysisState(PanelAnalysisState state);
    PanelAnalysisState getAnalysisState() const noexcept { return analysisState_; }

    void setConfidenceOverlayEnabled(bool enabled);
    void setDraggable(const juce::String& dragId);

    void setLoading(bool loading);
    bool isLoading() const noexcept { return isLoading_; }

    /// Opt-in collapse (Mix/Master layout memory). A collapsible card shows a
    /// chevron in its header; clicking it hides the body (children) and the
    /// owner lays it out at collapsedHeight(). onToggle fires with the new
    /// state so the owner can persist it and re-lay-out. Non-collapsible cards
    /// (the default) are completely unaffected.
    void setCollapsible(bool collapsible, std::function<void(bool)> onToggle = {});
    void setCollapsed(bool collapsed);
    bool isCollapsed() const noexcept { return collapsed_; }
    static constexpr int collapsedHeight() noexcept { return kHeaderH; }

    juce::Rectangle<int> bodyArea() const;

    /// Dockable panels: shows a pop-out button in the header that calls
    /// `handler` (the owner moves the card into/out of a floating window via
    /// PanelDockManager). `floating` swaps the icon/name to "dock back".
    void setPopOutHandler(std::function<void()> handler);
    void setFloating(bool floating);
    bool isFloating() const noexcept { return floating_; }

    /// Resizable panels: shows a drag grip along the bottom edge. Dragging (or
    /// Up/Down on the focused grip) reports the requested height; -1 means
    /// "reset to default" (double-click / Delete). The owner applies it.
    void setHeightResizeHandler(std::function<void(int)> handler);

    /// VER-8: marks the card as experimental - built, but not yet checked on
    /// real audio (ui/shared/FeatureMaturity.h). Shows an "Experimental" badge
    /// beside the title whose tooltip names the outstanding check.
    void setExperimental(bool experimental, const juce::String& outstandingCheck);
    /// UX-1: the card's Explain / Audition / Fix row. `codes` are the finding
    /// codes (prefixes) the card shows; the row sits under the body while one
    /// of them is open, and bodyArea() leaves room for it. Empty: no row.
    void setFindingCodes(std::vector<std::string> codes);
    CardFindingRow* findingRowForTesting() noexcept { return findingRow_.get(); }
    bool isExperimental() const noexcept { return experimental_; }
    juce::Component* experimentalBadgeForTesting() noexcept { return &experimentalBadge_; }
    juce::Rectangle<int> getHeaderBoundsForTesting() const { return getLocalBounds().removeFromTop(kHeaderH); }

    /// Standardized inline empty state (typography-audit rollout): renders the
    /// shared KhensuEmptyState visual into the body area. Panels call this from
    /// their "no data yet" paint branch instead of ad-hoc drawText, so every
    /// panel's empty state looks identical. Pass an optional top offset to sit
    /// below a controls row.
    void drawEmptyState(juce::Graphics& g, const juce::String& headline, const juce::String& body,
                        int topOffset = 0) const;

    void paint(juce::Graphics& g) override;
    void resized() override;
    /// Keeps a child added *while collapsed* hidden - setCollapsed() can only
    /// walk the children that existed when it ran (kimi audit P1 #17).
    void childrenChanged() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;

  protected:
    PanelCardController& controller_;

  private:
    /// Header icon button: a box with an outward arrow (pop out) or inward
    /// arrow (dock back). Drawn, not a glyph, to avoid font mojibake.
    struct PopOutButton final : public juce::Button {
        PopOutButton() : juce::Button("Pop out panel") {}
        bool docked = true;
        void paintButton(juce::Graphics& g, bool over, bool down) override;
    };
    /// Bottom-edge grip for height resizing.
    struct ResizeGrip final : public juce::Component, public juce::SettableTooltipClient {
        std::function<void(int)> onRequest; ///< absolute height request, -1 = reset
        int startHeight = 0;
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseDoubleClick(const juce::MouseEvent& e) override;
        bool keyPressed(const juce::KeyPress& key) override;
    };
    PopOutButton popOutButton_;
    ResizeGrip resizeGrip_;
    /// VER-8 badge (ui/shared/ExperimentalBadge.h): its tooltip carries the check.
    ExperimentalBadge experimentalBadge_;
    bool experimental_ = false;
    std::function<void()> popOutHandler_;
    bool floating_ = false;
    std::unique_ptr<CardFindingRow> findingRow_; ///< UX-1, see setFindingCodes
    bool isChromeChild(const juce::Component* c) const noexcept
    {
        return c == &explainerButton_ || c == &popOutButton_ || c == &experimentalBadge_ ||
               (findingRow_ != nullptr && c == (const juce::Component*)findingRow_.get());
    }

    juce::String title_;
    juce::TextButton explainerButton_{"?"};
    juce::String explCategory_, explDescription_, systemBrainFeatureId_;
    bool hasExplainer_ = false;
    PanelAnalysisState analysisState_ = PanelAnalysisState::NotApplicable;
    bool confidenceOverlayEnabled_ = false;
    static constexpr int kHeaderH = 30;

    bool draggable_ = false;
    juce::String dragId_;
    bool dragArmed_ = false;

    bool collapsible_ = false;
    bool collapsed_ = false;
    std::function<void(bool)> onCollapseToggle_;
    juce::Rectangle<int> chevronBounds() const;

    KhensuSkeleton skeleton_;
    bool isLoading_ = false;

    /// Re-applies the palette to widgets whose colours are cached rather than
    /// read live in paint(). Called at construction and on every theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PanelCard)
};

} // namespace khensu
