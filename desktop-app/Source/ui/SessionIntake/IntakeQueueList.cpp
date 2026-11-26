// desktop-app/Source/ui/SessionIntake/IntakeQueueList.cpp
#include "IntakeQueueList.h"
#include "../shared/KhensuTheme.h"
#include "../shared/KhensuLookAndFeel.h"

namespace khensu {

namespace {

juce::String formatDuration(float seconds)
{
    const int durSec = (int)std::lround(seconds);
    return juce::String::formatted("%d:%02d", durSec / 60, durSec % 60);
}

juce::String formatSpec(const AudioFileRef& ref)
{
    juce::String s = juce::String(ref.sample_rate / 1000.0, 1) + " kHz";
    s += (ref.channels == 1 ? " mono" : ref.channels == 2 ? " stereo" : " " + juce::String(ref.channels) + "ch");
    if (ref.bit_depth > 0) s += " / " + juce::String(ref.bit_depth) + "-bit";
    return s;
}

juce::String displayName(const AudioFileRef& ref)
{
    if (!ref.title.empty()) return juce::String(ref.title);
    return juce::File(ref.path).getFileName();
}

juce::String formatBadge(const AudioFileRef& ref)
{
    if (ref.format.empty()) return juce::File(ref.path).getFileExtension().toUpperCase().trimCharactersAtStart(".");
    return juce::String(ref.format).toUpperCase();
}

} // namespace

struct IntakeQueueList::Row final : public juce::Component {
    Row(IntakeQueueList& owner, int index) : owner_(owner), index_(index)
    {
        cancelButton_.setButtonText(TRANS("x"));
        applyThemeColours();
        cancelButton_.onClick = [this] {
            if (owner_.onCancel) owner_.onCancel(index_);
        };
        addAndMakeVisible(cancelButton_);
    }

    /// Rows are per-queue-item children rebuilt only when the queue changes, so
    /// each owns its listener - otherwise a theme switch over a static queue
    /// would never reach them (UI-01).
    void applyThemeColours()
    {
        cancelButton_.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        cancelButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textTertiary);
    }

    void paint(juce::Graphics& g) override
    {
        const bool selected = owner_.dragInsertIndex_ == index_ || owner_.dragInsertIndex_ == index_ + 1;
        auto b = getLocalBounds().toFloat();

        // Background
        g.setColour(selected ? KhensuTheme::bgRaised.brighter(0.04f) : KhensuTheme::bgSurface);
        g.fillRoundedRectangle(b.reduced(0.0f, 2.0f), 6.0f);

        // Drag handle
        g.setFont(KhensuTheme::bodyFont(type::secondary));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(juce::CharPointer_UTF8("\xe2\x8b\x80\xe2\x8b\x80"), b.removeFromLeft(26.0f).reduced(4.0f, 0.0f),
                   juce::Justification::centred, false);

        // Format badge
        auto badge = b.removeFromLeft(44.0f).reduced(2.0f, 14.0f);
        g.setColour(KhensuTheme::bgOverlay);
        g.fillRoundedRectangle(badge, 3.0f);
        g.setFont(KhensuTheme::monoFont(type::caption));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(formatBadge(owner_.files_[(size_t)index_]), badge, juce::Justification::centred, false);
        b.removeFromLeft(8.0f);

        // Filename + spec
        auto top = b.removeFromTop(b.getHeight() * 0.55f);
        g.setFont(KhensuTheme::bodyFont(type::secondary));
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(displayName(owner_.files_[(size_t)index_]), top, juce::Justification::centredLeft, true);

        g.setFont(KhensuTheme::monoFont(type::caption));
        g.setColour(KhensuTheme::textTertiary);
        const auto& ref = owner_.files_[(size_t)index_];
        juce::String meta = formatDuration(static_cast<float>(ref.duration_seconds)) + "  " + formatSpec(ref);
        g.drawText(meta, b, juce::Justification::centredLeft, true);

        // Status badge (drawn over the right side before the cancel button)
        const auto status = owner_.status_[(size_t)index_];
        juce::String stext = owner_.statusText(status);
        const float sw = juce::GlyphArrangement::getStringWidth(KhensuTheme::labelFont(type::caption), stext) + 18.0f;
        auto sb = getLocalBounds().removeFromRight((int)sw + 34).reduced(4, 12);
        g.setColour(owner_.statusColour(status).withAlpha(0.12f));
        g.fillRoundedRectangle(sb.toFloat(), 4.0f);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.setColour(owner_.statusColour(status));
        g.drawText(stext, sb, juce::Justification::centred, false);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        cancelButton_.setBounds(b.removeFromRight(28).reduced(2, 14));
    }

    void mouseDown(const juce::MouseEvent&) override { dragStarted_ = false; }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragStarted_ || !e.mouseWasDraggedSinceMouseDown()) return;
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this)) {
            dragStarted_ = true;
            container->startDragging("intake:" + juce::String(index_), this);
        }
    }

    IntakeQueueList& owner_;
    int index_;
    juce::TextButton cancelButton_;
    bool dragStarted_ = false;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};
};

IntakeQueueList::IntakeQueueList()
{
    addAndMakeVisible(viewport_);
    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);
}

IntakeQueueList::~IntakeQueueList() = default;

void IntakeQueueList::setFiles(const std::vector<AudioFileRef>& files)
{
    files_ = files;
    status_.assign(files_.size(), Status::Pending);
    rebuildRows();
}

void IntakeQueueList::setFilesAndPreserveStatus(const std::vector<AudioFileRef>& files)
{
    std::vector<Status> oldStatus = std::move(status_);
    setFiles(files);
    for (size_t i = 0; i < files_.size(); ++i) {
        // Try to preserve status for files that existed before.
        for (size_t j = 0; j < oldStatus.size(); ++j) {
            if (files_[i].path == files[j].path) {
                status_[i] = oldStatus[j];
                break;
            }
        }
    }
}

void IntakeQueueList::setStatus(int row, Status status)
{
    if (row < 0 || row >= (int)status_.size()) return;
    status_[(size_t)row] = status;
    if (row < (int)rows_.size()) rows_[(size_t)row]->repaint();
}

IntakeQueueList::Status IntakeQueueList::getStatus(int row) const
{
    if (row < 0 || row >= (int)status_.size()) return Status::Pending;
    return status_[(size_t)row];
}

void IntakeQueueList::clear()
{
    files_.clear();
    status_.clear();
    rebuildRows();
}

void IntakeQueueList::rebuildRows()
{
    rows_.clear();
    content_.removeAllChildren();
    for (size_t i = 0; i < files_.size(); ++i) {
        auto row = std::make_unique<Row>(*this, (int)i);
        content_.addAndMakeVisible(row.get());
        rows_.push_back(std::move(row));
    }
    resized();
    repaint();
}

juce::Colour IntakeQueueList::statusColour(Status s) const
{
    switch (s) {
    case Status::Pending:
        return KhensuTheme::textTertiary;
    case Status::Analyzing:
        return KhensuTheme::accentTeal;
    case Status::Done:
        return KhensuTheme::accentGreen;
    case Status::Failed:
        return KhensuTheme::accentRed;
    case Status::Cancelled:
        return KhensuTheme::accentAmber;
    }
    return KhensuTheme::textTertiary;
}

juce::String IntakeQueueList::statusText(Status s) const
{
    switch (s) {
    case Status::Pending:
        return "Pending";
    case Status::Analyzing:
        return "Analyzing";
    case Status::Done:
        return "Done";
    case Status::Failed:
        return "Failed";
    case Status::Cancelled:
        return "Cancelled";
    }
    return "";
}

void IntakeQueueList::paint(juce::Graphics& g) { g.fillAll(KhensuTheme::bgBase); }

void IntakeQueueList::resized()
{
    auto b = getLocalBounds();
    viewport_.setBounds(b);
    const int h = juce::jmax(b.getHeight(), (int)files_.size() * kRowH);
    content_.setBounds(0, 0, b.getWidth(), h);
    for (size_t i = 0; i < rows_.size(); ++i)
        rows_[(size_t)i]->setBounds(0, (int)i * kRowH, content_.getWidth(), kRowH);
}

bool IntakeQueueList::isInterestedInDragSource(const SourceDetails& details)
{
    return details.description.toString().startsWith("intake:");
}

void IntakeQueueList::itemDragEnter(const SourceDetails& details) { itemDragExit(details); }

void IntakeQueueList::itemDragExit(const SourceDetails&)
{
    if (dragInsertIndex_ != -1) {
        dragInsertIndex_ = -1;
        repaint();
    }
}

void IntakeQueueList::itemDropped(const SourceDetails& details)
{
    dragInsertIndex_ = -1;
    juce::String source = details.description.toString();
    const int from = source.fromFirstOccurrenceOf("intake:", false, false).getIntValue();
    const juce::Point<int> pos = details.localPosition;
    int to = pos.y / kRowH;
    to = juce::jlimit(0, (int)files_.size() - 1, to);
    if (from != to && from >= 0 && from < (int)files_.size() && onReorder) onReorder(from, to);
    repaint();
}

} // namespace khensu
