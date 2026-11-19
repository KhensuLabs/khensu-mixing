// desktop-app/Source/diagnostics/DiagnosticsBundle.h
#pragma once
#include <JuceHeader.h>
#include <utility>
#include <vector>

namespace khensu::diagnostics {

/// CE-ENG-4: "Export diagnostics bundle" - one zip a user can attach to a bug
/// report by hand. Built entirely on this machine and never uploaded.
///
/// Contents: crash records (text records, logs, and the newest few
/// minidumps), service logs, the opt-in performance trace, a versions file,
/// the settings with every credential redacted, and a manifest that lists
/// what was included and what was left out and why.
///
/// The service's bearer token is NEVER included:
///  * no file named token.txt is ever added, wherever it sits;
///  * the config directory itself is never walked - only named files and the
///    crashes / traces / logs subfolders are read;
///  * every added file is scanned for the token's text (ASCII and UTF-16):
///    text files are added with it replaced by "<redacted>", binary files
///    (a minidump can hold a copy of process memory) are left out entirely.
/// Builds even when the analysis service is down - that is exactly when a
/// bundle is needed.
struct BundleInputs {
    juce::File configDir;  ///< holds token.txt / settings.json (only named files are read)
    juce::File crashesDir; ///< minidumps + crash records (desktop, plugin, service)
    juce::File tracesDir;  ///< opt-in Chrome traces (trace.json)
    juce::File logsDir;    ///< desktop-side logs (terminate / service-crash captures)
    juce::var redactedSettings; ///< OnlineSettings::redactedJson()
    juce::String versionsText;  ///< app / service / OS versions
    /// The service's own diagnostics, fetched by the caller (REF-9): file
    /// name -> JSON text, written under service/ and token-scrubbed like
    /// every other text file. An empty text means the service did not answer;
    /// the manifest says so.
    std::vector<std::pair<juce::String, juce::String>> serviceSnapshots;
    int maxDumps = 3;           ///< newest minidumps to include
    juce::int64 maxFileBytes = 64 * 1024 * 1024;
};

struct BundleResult {
    bool ok = false;
    juce::File file;
    int filesAdded = 0;
    juce::StringArray skipped; ///< "name: reason"
    juce::String error;
};

/// Writes the zip to `destination` (overwritten). Blocking file I/O - call
/// from a background job.
BundleResult writeBundle(const BundleInputs& in, const juce::File& destination);

/// True when `data` contains `secret` as ASCII/UTF-8 or UTF-16LE bytes.
bool containsSecret(const juce::MemoryBlock& data, const juce::String& secret);

/// App / expected service / OS / CPU lines for versions.txt.
juce::String describeVersions(const juce::String& appVersion, const juce::String& serviceVersion);

} // namespace khensu::diagnostics
