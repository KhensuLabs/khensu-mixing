// desktop-app/Source/settings/OnlineSettings.h
#pragma once
#include <JuceHeader.h>
#include <functional>

namespace khensu {

/// Opt-out online-feature settings, persisted as settings.json in the shared
/// config dir (alongside token.txt). Defaults: everything on, no AcoustID key.
struct OnlineSettings {
    bool enabled = true;
    bool acoustidLookup = true;
    bool llmCopilot = true;
    bool modelDownloads = true;
    bool hfHub = true;
    // Release check (GET /updates/check), at launch and on demand. Gated by
    // `enabled` like every online feature.
    bool updateCheck = true;
    // v1.0.0: download a newer release's installer in the background and
    // install it when Khensu restarts (updates.auto_download).
    bool autoDownloadUpdates = true;
    // CE-COL-3: LAN listening room - the one opt-IN toggle (it opens a port
    // to the local network), so it defaults OFF. Also needs `enabled`.
    bool lanListening = false;
    // CE-COL-4: browsing a shared reference-profile index URL (opt-out).
    bool referenceIndex = true;
    juce::String releasesUrl;             ///< updates.releases_url override (https only; empty = default)
    juce::String dismissedUpdateVersion;  ///< the release whose notice the user dismissed
    juce::String acoustidApiKey;
    juce::String llmProvider{"local"};
    juce::String llmModel;
    // Context minimization for cloud-routed chat (audit DATA-03): when false,
    // only the message + history are sent - no analysis findings, genre, BPM,
    // or current chain. Local routing ignores this (nothing leaves anyway).
    bool llmShareSessionContext = true;
    // CE-AG-2: a GGUF model run in-process (provider "llamacpp") and an extra
    // loopback OpenAI-compatible server URL. Local only - never online-gated.
    juce::String llmGgufPath;
    juce::String llmLocalUrl;
    juce::String anthropicApiKey, geminiApiKey, openaiApiKey, deepseekApiKey, moonshotApiKey;
    // Local hardware preference. It shares settings.json so device selection
    // survives restarts, but is never governed by the online master toggle.
    juce::String midiInputIdentifier;
    // Local rendering-engine preference (Windows only): true = Direct2D GPU
    // context (JUCE 8's default), false = software GDI fallback for a driver/
    // GPU combination that hits z-order punch-through or glitches. Shares
    // settings.json but, like midiInputIdentifier, is never online-gated.
    bool useGpuRendering = true;
    // UX-5: the session-health strip (break reminders) is opt-in. Desktop
    // only, like useGpuRendering; persisted as ui.session_health_strip.
    bool showSessionHealthStrip = false;
    // VIS-7: reduced motion - "" follows Windows' "Show animations", "on" /
    // "off" is the user's choice (ui/shared/ReducedMotion.h). Desktop only;
    // persisted as ui.reduced_motion.
    juce::String reducedMotion;
    // LG-4: animated liquid smoke backdrop (paused automatically under
    // reduced motion or when unfocused). Desktop only; persisted as ui.ambient_backdrop.
    bool ambientBackdrop = true;
    // ML inference device preference (CE-PLT-1): "auto" | "cpu" | "gpu",
    // persisted as compute.accelerator and read by the service's
    // common/accel.py. Local; never online-gated.
    juce::String accelMode{"auto"};
    // NEW-10: what the user listens on (a monitoring::Profile id; "" = not
    // stated). Findings below what it reproduces are marked. Local; persisted
    // as listening.monitoring.
    juce::String monitoring;
    // C2PA content credentials (CE-DEL-2): the user's signing certificate
    // chain (PEM) and private key (PEM) paths, plus an optional RFC 3161
    // time-stamp authority URL (only contacted when online features are on).
    // Persisted as provenance.cert_path / key_path / tsa_url and read by the
    // service's analysis/c2pa_credentials.py. Paths only - never the key.
    juce::String provenanceCertPath, provenanceKeyPath, provenanceTsaUrl;
    // Automatic daily database backup (audit DATA-02). Opt-in - backups
    // consume disk. When enabled, AppController backs khensu.db up into
    // <configDir>/backups/ at most once per day (checked at launch and
    // hourly), keeping the newest 7. Local-only; never online-gated.
    bool scheduledBackup = false;
    // CE-ENG-4: opt-in Chrome-trace performance spans in the analysis service
    // (diagnostics.perf_traces; written locally to <local app data>/traces).
    bool perfTraces = false;
    juce::String lastScheduledBackupIso; ///< ISO-8601 of the last SUCCESSFUL scheduled backup

    /// How the current values were obtained (audit F9). CorruptFallback means a
    /// settings.json existed but could not be parsed, so opt-out defaults (all
    /// online ON) were silently applied — worth surfacing to the user, since it
    /// can re-enable features they had turned off. FreshDefaults (no file yet)
    /// is normal first-run and needs no warning.
    enum class Source { FreshDefaults, File, CorruptFallback };
    Source source = Source::FreshDefaults;

    static OnlineSettings load(const juce::File& configDir);
    bool save(const juce::File& configDir) const;

    /// Read-modify-write (DSK-11): loads the CURRENT settings.json, lets `fn`
    /// change only the fields its caller owns, and saves - so a writer never
    /// overwrites fields someone else changed since it loaded its copy (the
    /// Settings screen used to save the whole struct it read at
    /// construction, resetting the scheduled-backup timestamp and the
    /// dismissed-update version). Serialised in-process. Returns the saved
    /// settings (also on a failed write, with `ok` = false).
    static OnlineSettings update(const juce::File& configDir, const std::function<void(OnlineSettings&)>& fn,
                                 bool* ok = nullptr);

    /// Returns settings suitable for diagnostics/support exports. Provider
    /// credentials are represented only by an explicit redaction marker when
    /// configured; no encrypted or legacy plaintext value is copied out.
    juce::var redactedJson() const;

    /// Writes redactedJson() to a caller-selected support/diagnostic file.
    /// This is intentionally separate from save(), which must retain the
    /// encrypted-at-rest values needed by the application.
    bool saveRedactedTo(const juce::File& destination) const;

  private:
    juce::var toJson(bool redactSecrets, const juce::File& configDir) const;
};

/// True if these settings currently route copilot chat to a cloud provider
/// (Anthropic/Gemini/OpenAI-compatible) rather than the local heuristic/
/// local-LLM fallback (audit DATA-03) - the gate for the just-in-time
/// cloud-chat disclosure in ChatConsolePanel::sendMessage(). Pure function,
/// split out of AppController::isCloudCopilotConfigured() so it's
/// unit-testable without a live AppController.
inline bool isCloudProviderSelected(const OnlineSettings& settings) noexcept
{
    // CE-AG-2: the local choices never leave the machine; "auto" may fall
    // back to a cloud provider, so it still counts as cloud for the disclosure.
    const auto& p = settings.llmProvider;
    const bool localOnly = p == "local" || p == "ollama" || p == "lmstudio" || p == "llamacpp_server" ||
                           p == "llamacpp" || p == "local_server";
    return settings.enabled && settings.llmCopilot && p.isNotEmpty() && !localOnly;
}

/// Human-readable name of a configured LLM provider, for the cloud-chat
/// disclosure (audit DATA-03).
inline juce::String providerDisplayName(const juce::String& provider) noexcept
{
    const auto p = provider.trim().toLowerCase();
    if (p == "anthropic") return "Anthropic (Claude)";
    if (p == "gemini") return "Google Gemini";
    if (p == "deepseek") return "DeepSeek";
    if (p == "kimi" || p == "moonshot") return "Moonshot (Kimi)";
    if (p == "openai_compatible" || p == "openai") return "an OpenAI-compatible endpoint";
    return provider;
}

/// Provider-specific privacy-policy URL for the just-in-time cloud-chat
/// disclosure (audit DATA-03), or empty for an unknown/self-hosted provider
/// (an OpenAI-compatible base URL can point anywhere, so there's no single
/// canonical policy to link).
inline juce::String providerPrivacyPolicyUrl(const juce::String& provider) noexcept
{
    const auto p = provider.trim().toLowerCase();
    if (p == "anthropic") return "https://www.anthropic.com/legal/privacy";
    if (p == "gemini") return "https://policies.google.com/privacy";
    if (p == "deepseek") return "https://cdn.deepseek.com/policies/en-US/deepseek-privacy-policy.html";
    if (p == "kimi" || p == "moonshot") return "https://platform.moonshot.cn/docs/agreement/modules/privacy";
    return {};
}

} // namespace khensu
