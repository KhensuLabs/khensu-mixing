// desktop-app/Source/ui/SessionIntake/DropZoneComponent.cpp
#include "DropZoneComponent.h"
#include "../shared/KhensuTheme.h"
#include "../shared/KhensuLookAndFeel.h"

namespace khensu {

bool DropZoneComponent::isSupportedAudio(const juce::File& f)
{
    auto ext = f.getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".aiff" || ext == ".aif" || ext == ".flac" || ext == ".mp3" || ext == ".m4a" ||
           ext == ".ogg";
}

void DropZoneComponent::setLoaded(const juce::String& fn, const juce::String& fmt, const juce::String& dur,
                                  const juce::String& spec)
{
    filename_ = fn;
    format_ = fmt;
    duration_ = dur;
    spec_ = spec;
    state_ = State::Loaded;
    repaint();
}

void DropZoneComponent::clear()
{
    state_ = State::Empty;
    filename_ = format_ = duration_ = spec_ = {};
    repaint();
}

bool DropZoneComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files)
        if (isSupportedAudio(juce::File(f))) return true;
    return false;
}

void DropZoneComponent::fileDragEnter(const juce::StringArray&, int, int)
{
    state_ = State::DragOver;
    repaint();
}

void DropZoneComponent::fileDragExit(const juce::StringArray&)
{
    state_ = filename_.isEmpty() ? State::Empty : State::Loaded;
    repaint();
}

void DropZoneComponent::filesDropped(const juce::StringArray& files, int, int)
{
    std::vector<juce::File> audioFiles;
    for (const auto& file : files) {
        juce::File f(file);
        if (isSupportedAudio(f)) audioFiles.push_back(f);
    }
    if (!audioFiles.empty() && onFilesChosen) {
        onFilesChosen(audioFiles);
    }
    // State will be set to Loaded by the consumer calling setLoaded().
    if (state_ == State::DragOver) state_ = filename_.isEmpty() ? State::Empty : State::Loaded;
    repaint();
}

void DropZoneComponent::mouseDown(const juce::MouseEvent&)
{
    if (state_ == State::Loaded) return; // click on loaded state = ignore (use x button)
    browse();
}

void DropZoneComponent::browse()
{
    chooser_ = std::make_unique<juce::FileChooser>("Select audio file(s)", juce::File{},
                                                   "*.wav;*.aiff;*.aif;*.flac;*.mp3;*.m4a;*.ogg");
    auto browseFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
                       juce::FileBrowserComponent::canSelectMultipleItems;
    chooser_->launchAsync(browseFlags, [this](const juce::FileChooser& fc) {
        auto results = fc.getResults();
        std::vector<juce::File> audioFiles;
        for (const auto& file : results) {
            if (file.existsAsFile() && isSupportedAudio(file)) audioFiles.push_back(file);
        }
        if (!audioFiles.empty() && onFilesChosen) onFilesChosen(audioFiles);
    });
}

void DropZoneComponent::paint(juce::Graphics& g)
{
    if (state_ == State::Loaded) {
        // Collapsed 60px strip: glass panel + format badge + filename + spec/duration
        KhensuLookAndFeel::drawGlassPanel(g, getLocalBounds(), GlassPanelVariant::Raised);
        auto b = getLocalBounds().reduced(KhensuTheme::sp4, KhensuTheme::sp2);

        // Format badge
        auto badgeArea = b.removeFromLeft(44).reduced(0, 4);
        g.setColour(KhensuTheme::bgOverlay);
        g.fillRoundedRectangle(badgeArea.toFloat(), 3.0f);
        g.setFont(KhensuTheme::monoFont(10.0f));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(format_, badgeArea, juce::Justification::centred, false);

        b.removeFromLeft(KhensuTheme::sp2);

        // Filename (top half) + spec - duration (bottom half)
        g.setFont(KhensuTheme::bodyFont(12.0f));
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(filename_, b.removeFromTop(b.getHeight() / 2), juce::Justification::centredLeft, true);

        g.setFont(KhensuTheme::monoFont(10.0f));
        g.setColour(KhensuTheme::textTertiary);
        juce::String meta = spec_.isEmpty() ? duration_ : (spec_ + "  " + duration_);
        g.drawText(meta, b, juce::Justification::centredLeft, true);
        return;
    }

    // ── Empty / DragOver ─────────────────────────────────────────────────────
    bool dragOver = (state_ == State::DragOver);
    KhensuLookAndFeel::drawGlassPanel(g, getLocalBounds(),
                                      dragOver ? GlassPanelVariant::GlowAccent : GlassPanelVariant::Default);

    // Dashed border
    auto bounds = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(dragOver ? KhensuTheme::accentTeal : KhensuTheme::borderSubtle);
    float dashes[] = {6.0f, 4.0f};
    juce::Path borderShape;
    borderShape.addRoundedRectangle(bounds, 6.0f);
    juce::Path dashedBorder;
    juce::PathStrokeType(1.0f).createDashedStroke(dashedBorder, borderShape, dashes, 2);
    g.fillPath(dashedBorder); // createDashedStroke() yields a filled outline

    if (dragOver) {
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.10f));
        g.fillRoundedRectangle(bounds.reduced(1.0f), 6.0f);
    }

    // Centre labels
    auto centre = getLocalBounds().reduced(KhensuTheme::sp7);
    g.setFont(KhensuTheme::bodyFont(13.0f));
    g.setColour(dragOver ? KhensuTheme::textPrimary : KhensuTheme::textSecondary);
    g.drawText("Drop audio file here", centre.removeFromTop(centre.getHeight() / 2), juce::Justification::centredBottom,
               false);
    g.setFont(KhensuTheme::labelFont(11.0f));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText("or click to browse", centre, juce::Justification::centredTop, false);
}

} // namespace khensu
