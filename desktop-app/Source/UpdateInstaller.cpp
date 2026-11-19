// desktop-app/Source/UpdateInstaller.cpp
#include "UpdateInstaller.h"
#include <mutex>

namespace khensu::update_installer {
namespace {
std::mutex g_mutex;
juce::File g_installer;
juce::String g_version;
bool g_launched = false;

juce::File runningExecutable()
{
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile);
}

bool underProgramFiles(const juce::File& exe)
{
#if JUCE_WINDOWS
    for (auto loc : {juce::File::globalApplicationsDirectory, juce::File::globalApplicationsDirectoryX86})
        if (exe.isAChildOf(juce::File::getSpecialLocation(loc))) return true;
#else
    juce::ignoreUnused(exe);
#endif
    return false;
}
} // namespace

bool isAcceptableInstaller(const juce::File& installer)
{
    if (!installer.existsAsFile()) return false;
    if (installer.getParentDirectory().getFileName() != "updates") return false;
    const auto name = installer.getFileName().toLowerCase();
#if JUCE_MAC
    return name.startsWith("khensu") && name.endsWith(".pkg");
#else
    return name.startsWith("khensu-setup-") && name.endsWith(".exe");
#endif
}

bool setPending(const juce::File& installer, const juce::String& version)
{
    if (!isAcceptableInstaller(installer)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_installer = installer;
    g_version = version;
    return true;
}

juce::File pendingInstaller()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_installer;
}

juce::String pendingVersion()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_version;
}

void clearPending()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_installer = juce::File();
    g_version.clear();
}

bool isInstalledCopy(const juce::File& exe)
{
#if JUCE_MAC
    // .../Applications/Khensu Mixing.app/Contents/MacOS/Khensu Mixing
    const auto bundle = exe.getParentDirectory().getParentDirectory().getParentDirectory();
    return bundle.getFileExtension() == ".app" && bundle.getParentDirectory().getFileName() == "Applications";
#else
    return exe.getSiblingFile("unins000.exe").existsAsFile();
#endif
}

juce::StringArray windowsInstallerArgs(bool relaunch, bool allUsers, const juce::File& log)
{
    // Inno Setup: no wizard, no prompts, no reboot; it reuses the previous
    // install's folders and components (UsePreviousAppDir and the plugin
    // folder page's saved choice), and closes whatever still runs from them.
    juce::StringArray args{"/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-",
                           allUsers ? "/ALLUSERS" : "/CURRENTUSER",
                           "/LOG=" + log.getFullPathName().quoted()};
    if (relaunch) args.add("/RELAUNCH=1");
    return args;
}

bool launchPending(bool relaunch)
{
    juce::File installer;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_launched || g_installer == juce::File()) return false;
        installer = g_installer;
    }
    const auto exe = runningExecutable();
    if (!isAcceptableInstaller(installer) || !isInstalledCopy(exe)) return false;

    bool started = false;
#if JUCE_WINDOWS
    const auto log = installer.getSiblingFile("install-" + installer.getFileNameWithoutExtension() + ".log");
    started = installer.startAsProcess(windowsInstallerArgs(relaunch, underProgramFiles(exe), log).joinIntoString(" "));
#elif JUCE_MAC
    // Installs for the current user (no administrator prompt) after this
    // process has gone, then reopens the app when asked to. UNVERIFIED on a Mac.
    const auto app = exe.getParentDirectory().getParentDirectory().getParentDirectory();
    juce::String script = "sleep 2; /usr/sbin/installer -pkg " + installer.getFullPathName().quoted('\'') +
                          " -target CurrentUserHomeDirectory";
    if (relaunch) script << " && /usr/bin/open " << app.getFullPathName().quoted('\'');
    juce::ChildProcess child;
    started = child.start(juce::StringArray{"/bin/sh", "-c", script}, 0);
#endif
    if (started) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_launched = true;
    }
    return started;
}

} // namespace khensu::update_installer
