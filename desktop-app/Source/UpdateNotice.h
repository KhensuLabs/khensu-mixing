// desktop-app/Source/UpdateNotice.h
#pragma once
#include <JuceHeader.h>
#include <khensu/ipc/HttpServiceBridge.h>

namespace khensu::update_notice {

/// Whether the launch-time release check should put "Version X is available"
/// in front of the user: only for a real newer release, and never again for a
/// version the user already dismissed.
inline bool shouldShow(const ipc::HttpServiceBridge::UpdateCheckResult& result,
                       const juce::String& dismissedVersion) noexcept
{
    return result.updateAvailable() && result.latestVersion.isNotEmpty() &&
           result.latestVersion != dismissedVersion;
}

inline juce::String text(const ipc::HttpServiceBridge::UpdateCheckResult& result)
{
    return TRANS("Khensu Mixing") + " " + result.latestVersion + " " + TRANS("is available (you have") + " " +
           result.currentVersion + ").";
}

/// Only an https release page is ever opened from the notice.
inline bool hasSafeLink(const ipc::HttpServiceBridge::UpdateCheckResult& result) noexcept
{
    return result.url.startsWithIgnoreCase("https://");
}

/// SHIP-4: the release's installer, only over https. The notice opens it in
/// the browser, which downloads it; nothing runs without the user.
inline bool hasSafeDownload(const ipc::HttpServiceBridge::UpdateCheckResult& result) noexcept
{
    return result.downloadUrl.startsWithIgnoreCase("https://");
}

/// The downloaded update, waiting for a restart (v1.0.0).
inline juce::String readyText(const ipc::HttpServiceBridge::UpdateCheckResult& result)
{
    return TRANS("Khensu Mixing") + " " + result.latestVersion + " " +
           TRANS("is downloaded and ready. It installs when you close or restart Khensu.");
}

/// While the service downloads it.
inline juce::String downloadingText(const ipc::HttpServiceBridge::UpdateCheckResult& result)
{
    return TRANS("Downloading Khensu Mixing") + " " + result.latestVersion + " " +
           TRANS("in the background...");
}

/// What a manual "Check for updates" says when there is no newer release.
inline juce::String manualText(const ipc::HttpServiceBridge::UpdateCheckResult& result)
{
    if (result.updateAvailable()) return text(result);
    if (result.status == "up_to_date")
        return TRANS("Khensu Mixing") + " " + result.currentVersion + " " + TRANS("is up to date.");
    if (result.status == "disabled")
        return TRANS("Update checks are turned off. Turn on online features and \"Check for new versions\" in "
                     "Settings.");
    return TRANS("Could not reach the release page on GitHub. Check your internet connection and try again.");
}

} // namespace khensu::update_notice
