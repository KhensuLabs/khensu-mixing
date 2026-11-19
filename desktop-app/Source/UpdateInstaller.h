// desktop-app/Source/UpdateInstaller.h
#pragma once
#include <JuceHeader.h>

// Installs a downloaded update when the app restarts (v1.0.0, SHIP-4).
//
// The service downloads a newer release's installer to <config dir>/updates/
// and verifies it against the SHA-256 GitHub publishes. The desktop records
// it here as pending; when the app closes (or the user chooses "Restart
// now"), the installer runs silently into the same folders and, for "Restart
// now", opens the new version. Only an installed copy updates itself: a build
// run from a source tree never launches an installer.
namespace khensu::update_installer {

/// The installer the service reported ready, or {} - only a file inside an
/// "updates" folder named like a Khensu installer for this OS is accepted.
bool setPending(const juce::File& installer, const juce::String& version);
juce::File pendingInstaller();
juce::String pendingVersion();
void clearPending();

/// Whether `installer` looks like one the service downloaded for this OS.
bool isAcceptableInstaller(const juce::File& installer);

/// True for a copy installed by the Khensu installer: on Windows the exe sits
/// beside unins000.exe; on macOS the app bundle lives in an Applications folder.
bool isInstalledCopy(const juce::File& runningExecutable);

/// The silent-install arguments (Windows / Inno Setup). `allUsers` keeps an
/// elevated install under Program Files; `relaunch` opens the new version.
juce::StringArray windowsInstallerArgs(bool relaunch, bool allUsers, const juce::File& log);

/// Starts the pending installer as a detached process. Returns false (and does
/// nothing) when there is none, it is not acceptable, this copy is not an
/// installed one, or it was already launched in this run.
bool launchPending(bool relaunch);

} // namespace khensu::update_installer
