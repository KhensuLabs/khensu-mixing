// desktop-app/Source/BundledResources.h
// Where the app's bundled data files (resources/genre_norms, system_brain,
// calibration, fonts) are: beside the executable on Windows and in a dev
// build; in Contents/Resources inside a macOS .app (a loose file in
// Contents/MacOS breaks code signing - "code object is not signed at all");
// else the working directory (running from the repository).
#pragma once
#include <JuceHeader.h>

namespace khensu::bundled_resources {

/// The file or directory at `relative` (e.g. "resources/fonts"), or the
/// working-directory candidate when no bundled copy exists.
inline juce::File find(const juce::String& relative)
{
    const auto exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    auto candidate = exeDir.getChildFile(relative);
    if (candidate.exists()) return candidate;
#if JUCE_MAC
    candidate = exeDir.getSiblingFile("Resources").getChildFile(relative);
    if (candidate.exists()) return candidate;
#endif
    return juce::File::getCurrentWorkingDirectory().getChildFile(relative);
}

} // namespace khensu::bundled_resources
