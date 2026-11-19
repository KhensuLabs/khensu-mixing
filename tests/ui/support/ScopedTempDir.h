#pragma once
#include <JuceHeader.h>

namespace khensu::test {

/// A fresh folder under %TEMP%, deleted on destruction (PKG-4).
///
/// Declare it as a fixture's FIRST member: members are destroyed in reverse
/// order, so it goes after the services that keep khensu.db open. Deleting
/// in the fixture's destructor body ran while they still held the file, and
/// on Windows an open file cannot be deleted - 300+ folders piled up in %TEMP%.
struct ScopedTempDir {
    explicit ScopedTempDir(const juce::String& prefix)
        : file(juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile(prefix + juce::Uuid().toString()))
    {
        file.createDirectory();
    }
    ~ScopedTempDir() { file.deleteRecursively(); }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const juce::File file;
};

} // namespace khensu::test
