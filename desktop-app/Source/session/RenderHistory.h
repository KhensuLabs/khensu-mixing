// desktop-app/Source/session/RenderHistory.h
#pragma once
#include <JuceHeader.h>
#include <vector>

namespace khensu {

/// Audition history for applied copilot renders, extracted from AppController
/// (audit M4 — first slice of the SessionState decomposition). Pure state: no
/// playback, no file IO. AppController owns that and drives this type.
///
/// Two parallel structures, exactly as before the extraction:
///   * a linear undo/redo stack that truncates the redo tail on a new render
///     (standard undo semantics), and
///   * a branch-preserving tree that never truncates — every render stays a
///     node, and a node's parent is the audio it was rendered FROM: the
///     original (a root) or, since WF-1, the accepted render it processed
///     (registerRenderFrom()), so stacked fixes read as a chain.
///
/// currentIndex() == -1 means the original (unprocessed) source is current;
/// currentPath() returns "" in that state.
class RenderHistory {
  public:
    struct TreeNode {
        juce::String path;
        int parent = -1; ///< index into tree(), or -1 for a root
    };

    // ---- linear history ----
    const std::vector<juce::String>& applied() const noexcept { return applied_; }
    int currentIndex() const noexcept { return currentIndex_; }
    bool canUndo() const noexcept { return currentIndex_ >= 0; }
    bool canRedo() const noexcept { return currentIndex_ + 1 < (int)applied_.size(); }
    // Undo/redo also move the tree's current node: previously they left it
    // on the undone render, so the next render was parented to a node the
    // user had already stepped back from.
    void undo() noexcept
    {
        if (canUndo()) --currentIndex_;
        syncNodeToIndex();
    }
    void redo() noexcept
    {
        if (canRedo()) ++currentIndex_;
        syncNodeToIndex();
    }

    /// Current render path, or "" when the original source is current.
    juce::String currentPath() const
    {
        return (currentIndex_ >= 0 && currentIndex_ < (int)applied_.size()) ? applied_[(size_t)currentIndex_]
                                                                            : juce::String();
    }

    /// The file Export should write: the accepted (current) render while it
    /// exists on disk, else `originalPath`. Deliberately independent of the
    /// A/B "B" slot, which may hold an ABX blind reference, an unaccepted
    /// agent candidate or an audition preview (AUDIT-001).
    juce::String exportSource(const juce::String& originalPath) const
    {
        const juce::String accepted = currentPath();
        return accepted.isNotEmpty() && juce::File(accepted).existsAsFile() ? accepted : originalPath;
    }

    // ---- branch-preserving tree ----
    const std::vector<TreeNode>& tree() const noexcept { return tree_; }
    int currentNode() const noexcept { return currentNode_; }

    /// CE-PLT-2: the audio `node` was rendered FROM - its parent node's path,
    /// or `originalPath` for a root render. "" when `node` is out of range
    /// (the original itself has no baseline). A metric re-measure of the
    /// render passes this as `baseline_path` so only the changed windows are
    /// recomputed.
    static juce::String baselineFor(const std::vector<TreeNode>& tree, int node, const juce::String& originalPath)
    {
        if (node < 0 || node >= (int)tree.size()) return {};
        const int parent = tree[(size_t)node].parent;
        if (parent >= 0 && parent < (int)tree.size() && parent != node) return tree[(size_t)parent].path;
        return originalPath;
    }

    enum class Kept { Gone, Undone, Kept };

    /// Whether the render at `path` is part of the current version (FIX-3;
    /// taste learning counts only kept fixes, P0-2).
    /// Fixes stack (WF-1), so a render is in the audio only when the current
    /// node is that render or was built on it - an ancestor in the tree. Its
    /// place in the linear list says nothing: fix A, "Start from original",
    /// fix B leaves A before B in the list and out of the audio.
    /// Undone = still in the tree, so it can be restored; Gone = not in the
    /// tree at all (a session reset).
    static Kept keptInTree(const std::vector<TreeNode>& tree, int currentNode, const juce::String& path)
    {
        // The step count bounds the walk if a parent link is ever corrupt.
        int node = currentNode;
        for (size_t steps = 0; steps < tree.size() && node >= 0 && node < (int)tree.size(); ++steps) {
            if (tree[(size_t)node].path == path) return Kept::Kept;
            node = tree[(size_t)node].parent;
        }
        for (const auto& n : tree)
            if (n.path == path) return Kept::Undone;
        return Kept::Gone;
    }

    // ---- mutations ----
    /// Register a render whose input is not known as the current tip, as a
    /// child of the root (the original). Truncates any redo tail on the
    /// linear stack first.
    ///
    /// Renders made in the app process `renderInputPath()` and register with
    /// `registerRenderOf`, which parents them at the version they processed
    /// (WF-1). This root form is for restoring a known render (autosave,
    /// workspace recovery), where the chain that produced it is not recorded.
    void registerRender(const juce::String& path) { appendNode(path, -1); }

    /// Register a render that was rendered from the CURRENT render (a
    /// genuinely cumulative step): its tree node is parented at the current
    /// node, so the branch shows the processing chain it carries.
    void registerDerivedRender(const juce::String& path) { appendNode(path, currentNode_); }

    /// WF-1: register a render that processed `inputPath` - a child of the
    /// newest node holding that path (fixes stack), or a root when the input
    /// was the original (or a path the tree doesn't hold). Keyed on the
    /// input rather than on the current node, so a render that finishes
    /// after the user stepped to another version still lands under the
    /// audio it was actually made from.
    void registerRenderFrom(const juce::String& path, const juce::String& inputPath)
    {
        appendNode(path, inputPath.isEmpty() ? -1 : findNode(inputPath));
    }

    /// Tree index of the most recent node holding `path`, or -1.
    int findNode(const juce::String& path) const noexcept
    {
        for (int i = (int)tree_.size() - 1; i >= 0; --i)
            if (tree_[(size_t)i].path == path) return i;
        return -1;
    }

    /// Make a tree node current and re-sync the linear tip to its path (renders
    /// are never destroyed, so appending is safe). Returns the node's path, or
    /// "" for an out-of-range index — in which case nothing changes.
    juce::String restoreTreeNode(int nodeIndex)
    {
        if (nodeIndex < 0 || nodeIndex >= (int)tree_.size()) return {};
        currentNode_ = nodeIndex;
        const juce::String path = tree_[(size_t)nodeIndex].path;
        truncateRedoTail();
        applied_.push_back(path);
        appliedNodes_.push_back(nodeIndex);
        currentIndex_ = (int)applied_.size() - 1;
        return path;
    }

    void clear()
    {
        applied_.clear();
        appliedNodes_.clear();
        currentIndex_ = -1;
        tree_.clear();
        currentNode_ = -1;
    }

  private:
    void appendNode(const juce::String& path, int parent)
    {
        truncateRedoTail();
        tree_.push_back({path, parent});
        currentNode_ = (int)tree_.size() - 1;
        applied_.push_back(path);
        appliedNodes_.push_back(currentNode_);
        currentIndex_ = (int)applied_.size() - 1;
    }
    void truncateRedoTail()
    {
        if (currentIndex_ + 1 < (int)applied_.size()) {
            applied_.resize((size_t)currentIndex_ + 1);
            appliedNodes_.resize((size_t)currentIndex_ + 1);
        }
    }
    void syncNodeToIndex() noexcept
    {
        currentNode_ = (currentIndex_ >= 0 && currentIndex_ < (int)appliedNodes_.size())
                           ? appliedNodes_[(size_t)currentIndex_]
                           : -1;
    }

    std::vector<juce::String> applied_;
    std::vector<int> appliedNodes_; ///< tree node of each applied_ entry
    int currentIndex_ = -1;
    std::vector<TreeNode> tree_;
    int currentNode_ = -1;
};

} // namespace khensu
