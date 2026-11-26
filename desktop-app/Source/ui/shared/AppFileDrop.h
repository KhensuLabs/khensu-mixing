// desktop-app/Source/ui/shared/AppFileDrop.h
// UXR-10: what a drop anywhere in the window opens - the supported audio
// files among the dropped paths, in the order given (folders and other files
// are ignored). The extension list is Intake's drop zone's
// (DropZoneComponent::isSupportedAudio, private there); keep the two equal.
#pragma once
#include <JuceHeader.h>
#include <vector>

namespace khensu::app_file_drop {

inline bool isSupportedAudio(const juce::File& f)
{
    const auto ext = f.getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".aiff" || ext == ".aif" || ext == ".flac" || ext == ".mp3" || ext == ".m4a" ||
           ext == ".ogg";
}

inline std::vector<juce::File> audioFiles(const juce::StringArray& paths)
{
    std::vector<juce::File> out;
    for (const auto& path : paths) {
        const juce::File f(path);
        if (!f.isDirectory() && isSupportedAudio(f)) out.push_back(f);
    }
    return out;
}

} // namespace khensu::app_file_drop
