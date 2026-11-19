// desktop-app/Source/ui/SessionIntake/IntakeQueueList.h
// Reorderable, cancellable multi-file intake queue (shipped).
#pragma once
#include <JuceHeader.h>
#include <khensu/session/AudioFileRef.h>
#include <functional>
#include <memory>
#include <vector>

namespace khensu {

// THREAD_CONTEXT(UIThread)
class IntakeQueueList final : public juce::Component,
                              public juce::DragAndDropContainer,
                              public juce::DragAndDropTarget {
  public:
    enum class Status { Pending, Analyzing, Done, Failed, Cancelled };

    IntakeQueueList();
    ~IntakeQueueList() override;

    void setFiles(const std::vector<AudioFileRef>& files);
    void setStatus(int row, Status status);
    Status getStatus(int row) const;
    void clear();

    int getNumFiles() const noexcept { return (int)files_.size(); }
    const AudioFileRef& getFile(int row) const { return files_[(size_t)row]; }

    /// Replace the current file list while preserving status where possible.
    /// Used when the user reorders rows so the background thread can read
    /// the new order via getFiles().
    std::vector<AudioFileRef> getFiles() const { return files_; }
    void setFilesAndPreserveStatus(const std::vector<AudioFileRef>& files);

    std::function<void(int oldIndex, int newIndex)> onReorder;
    std::function<void(int index)> onCancel;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // juce::DragAndDropTarget
    bool isInterestedInDragSource(const SourceDetails& dragSourceDetails) override;
    void itemDragEnter(const SourceDetails& dragSourceDetails) override;
    void itemDragExit(const SourceDetails&) override;
    void itemDropped(const SourceDetails& dragSourceDetails) override;

  private:
    struct Row;

    void rebuildRows();
    juce::Colour statusColour(Status s) const;
    juce::String statusText(Status s) const;

    std::vector<AudioFileRef> files_;
    std::vector<Status> status_;
    int dragInsertIndex_ = -1;

    juce::Viewport viewport_;
    juce::Component content_;
    std::vector<std::unique_ptr<Row>> rows_;

    static constexpr int kRowH = 56;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IntakeQueueList)
};

} // namespace khensu
