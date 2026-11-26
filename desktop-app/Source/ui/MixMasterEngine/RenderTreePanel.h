// desktop-app/Source/ui/MixMasterEngine/RenderTreePanel.h
// Branching render history as a node tree - every applied render is a node
// whose parent is the render it was built on. Click a node to jump
// playback/A-B back to it (AppController::restoreRenderTreeNode).
//
// CE-COL-2 version control: the tree is synced to the service
// (render_versions, migration 014) so a node can be named (a restore point),
// tagged, branched from, compared with the current version and exported as a
// .khensu bundle; versions saved in an earlier run are listed below the live
// tree and restore with one click. Right-click a node for the actions.
#pragma once
#include <set>
#include <JuceHeader.h>
#include "../shared/ListenerHandle.h"
#include "../shared/PanelCard.h"
#include <map>
#include <vector>

namespace khensu {

class IAppController;

class RenderTreePanel final : public PanelCard, public juce::SettableTooltipClient {
  public:
    explicit RenderTreePanel(IAppController& c);
    ~RenderTreePanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void resetForSession() override;

    /// STU-8: plays the version at `treeIndex` as the A / B side of the
    /// loudness-matched A/B. Playback only - the accepted version is unchanged.
    void pinAs(bool sideA, int treeIndex);
    /// STU-8: back to the original against the current version.
    void resetAb();
    juce::TextButton& resetAbButtonForTesting() noexcept { return resetAbButton_; }

  private:
    struct NodeLayout {
        int index; ///< tree index, or -1 for a saved (earlier-run) version row
        int depth;
        juce::Rectangle<float> dot;
        juce::String savedPath; ///< for a saved-version row
    };
    struct VersionInfo {
        juce::String id, label, branch, parentId;
        juce::StringArray tags;
    };

    void rebuildLayout();
    void syncVersions();
    void applyVersions(const juce::var& response);
    void showNodeMenu(int treeIndex, const juce::String& path);
    void promptLabel(const VersionInfo& v);
    void promptBranch(int treeIndex, const VersionInfo& v);
    void exportBranch(const juce::String& branch);
    void compareWithCurrent(const juce::String& path);
    void restoreSaved(const juce::String& path);
    void setStatus(const juce::String& s);
    juce::String sessionId() const;
    juce::String headerText() const;

    std::vector<NodeLayout> layout_;
    int rowH_ = 28;
    /// VIZ-N4: each version's measured difference spectrum against the
    /// original, drawn as a sparkline at the row's end (filled in lazily).
    std::map<juce::String, std::vector<double>> diffByPath_;
    std::set<juce::String> diffPending_;
    void requestDiff(const juce::String& path);
    void paintDiff(juce::Graphics& g, juce::Rectangle<float> r, const std::vector<double>& diffDb) const;
    void mouseMove(const juce::MouseEvent& e) override;
    int savedHeaderY_ = -1;

    std::map<juce::String, VersionInfo> versionsByPath_;
    juce::StringArray branches_;
    juce::String activeBranch_;
    juce::String syncedSessionId_;
    bool syncInFlight_ = false;
    bool syncAgain_ = false;
    juce::String status_;

    std::unique_ptr<juce::AlertWindow> dialog_;
    std::unique_ptr<juce::FileChooser> chooser_;

    // STU-8: the row's "A" / "B" pin buttons (drawn; hit-tested in mouseDown)
    // and the Reset A/B button, shown while the A/B is not original vs current.
    static constexpr float kPinW = 22.0f, kPinH = 16.0f;
    juce::Rectangle<float> pinBounds(const NodeLayout& nl, bool sideA) const;
    juce::String abSummary() const;
    void updateResetButton();
    juce::TextButton resetAbButton_;

    // ENG-2: hub registrations; destroyed (deregistered) before the members
    // above them that the callbacks touch.
    ListenerHandle renderTreeListener_;
    ListenerHandle abAppliedListener_; ///< STU-8: B changed elsewhere - redraw the pins

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RenderTreePanel)
};

} // namespace khensu
