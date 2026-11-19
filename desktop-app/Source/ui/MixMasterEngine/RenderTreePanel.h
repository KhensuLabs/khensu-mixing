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
#include <JuceHeader.h>
#include "../shared/ListenerHandle.h"
#include "../shared/PanelCard.h"
#include <map>
#include <vector>

namespace khensu {

class IAppController;

class RenderTreePanel final : public PanelCard {
  public:
    explicit RenderTreePanel(IAppController& c);
    ~RenderTreePanel() override;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void resetForSession() override;

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
    int rowH_ = 22;
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

    // ENG-2: hub registrations; destroyed (deregistered) before the members
    // above them that the callbacks touch.
    ListenerHandle renderTreeListener_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RenderTreePanel)
};

} // namespace khensu
