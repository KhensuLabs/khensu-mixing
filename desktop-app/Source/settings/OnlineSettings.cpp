// desktop-app/Source/settings/OnlineSettings.cpp
#include "OnlineSettings.h"
#include <khensu/common/CredentialProtector.h>
#include <khensu/common/FileAcl.h>
#include <mutex>
#include <khensu/recommendations/MonitoringReach.h>

namespace khensu {

namespace {
std::string configDirUtf8(const juce::File& configDir)
{
    return configDir.getFullPathName().toStdString();
}
} // namespace

OnlineSettings OnlineSettings::load(const juce::File& configDir)
{
    OnlineSettings s;
    const auto file = configDir.getChildFile("settings.json");
    if (!file.existsAsFile()) return s; // FreshDefaults (normal first run)
    const auto parsed = juce::JSON::parse(file.loadFileAsString());
    if (parsed.getDynamicObject() != nullptr) {
        s.source = Source::File;
        const auto online = parsed["online"];
        if (online.isObject()) {
            auto b = [&online](const char* k, bool def) {
                const auto v = online[k];
                return v.isVoid() ? def : static_cast<bool>(v);
            };
            s.enabled = b("enabled", true);
            s.acoustidLookup = b("acoustid_lookup", true);
            s.llmCopilot = b("llm_copilot", true);
            s.modelDownloads = b("model_downloads", true);
            s.hfHub = b("hf_hub", true);
            s.updateCheck = b("update_check", true);
            s.lanListening = b("lan_listening", false);
            s.referenceIndex = b("reference_index", true);
        }
        // CredentialProtector::unprotect is transparent over legacy plaintext
        // (audit SEC-06): a settings.json from before this feature existed
        // still loads correctly. Such inline values are re-sealed behind a
        // credential reference right away (AUDIT-087: they used to stay
        // plaintext until the user happened to change a setting) - but only
        // if every one can be stored securely, so nothing is dropped.
        const auto configDirPath = configDirUtf8(configDir);
        bool resealWanted = false;
        bool resealPossible = true;
        auto loadSecret = [&](const char* slotId, const juce::var& raw) {
            const std::string stored = raw.toString().toStdString();
            std::string plain = CredentialProtector::unprotectFromSettings(slotId, stored, configDirPath);
            if (CredentialProtector::needsReseal(stored) && !plain.empty()) {
                resealWanted = true;
                if (CredentialProtector::protectForSettings(slotId, plain, configDirPath).empty())
                    resealPossible = false;
            }
            return juce::String(plain);
        };
        const auto ac = parsed["acoustid"];
        if (ac.isObject()) s.acoustidApiKey = loadSecret("acoustid.api_key", ac["api_key"]);
        const auto llm = parsed["llm"];
        if (llm.isObject()) {
            s.llmProvider = llm["provider"].toString();
            s.llmModel = llm["model"].toString();
            s.anthropicApiKey = loadSecret("llm.anthropic_api_key", llm["anthropic_api_key"]);
            s.geminiApiKey = loadSecret("llm.gemini_api_key", llm["gemini_api_key"]);
            s.openaiApiKey = loadSecret("llm.openai_api_key", llm["openai_api_key"]);
            s.deepseekApiKey = loadSecret("llm.deepseek_api_key", llm["deepseek_api_key"]);
            s.moonshotApiKey = loadSecret("llm.moonshot_api_key", llm["moonshot_api_key"]);
            const auto share = llm["share_session_context"];
            s.llmShareSessionContext = share.isVoid() ? true : static_cast<bool>(share);
            s.llmGgufPath = llm["gguf_path"].toString();   // CE-AG-2
            s.llmLocalUrl = llm["local_url"].toString();
        }
        const auto midi = parsed["midi"];
        if (midi.isObject()) s.midiInputIdentifier = midi["input_identifier"].toString();
        const auto ui = parsed["ui"];
        if (ui.isObject()) {
            s.showSessionHealthStrip = static_cast<bool>(ui["session_health_strip"]);
            const auto motion = ui["reduced_motion"].toString().trim().toLowerCase(); // VIS-7
            if (motion == "on" || motion == "off") s.reducedMotion = motion;
            const auto ambient = ui["ambient_backdrop"]; // LG-4
            s.ambientBackdrop = ambient.isVoid() ? true : static_cast<bool>(ambient);
        }
        const auto rendering = parsed["rendering"];
        if (rendering.isObject()) {
            const auto v = rendering["use_direct2d"];
            s.useGpuRendering = v.isVoid() ? true : static_cast<bool>(v);
        }
        const auto compute = parsed["compute"];
        if (compute.isObject()) {
            const auto mode = compute["accelerator"].toString().trim().toLowerCase();
            if (mode == "auto" || mode == "cpu" || mode == "gpu") s.accelMode = mode;
        }
        const auto listening = parsed["listening"]; // NEW-10
        if (listening.isObject()) {
            const auto id = listening["monitoring"].toString().trim().toLowerCase();
            if (std::string(monitoring::profile(id.toStdString()).id) == id.toStdString()) s.monitoring = id;
        }
        const auto provenance = parsed["provenance"]; // CE-DEL-2
        if (provenance.isObject()) {
            s.provenanceCertPath = provenance["cert_path"].toString();
            s.provenanceKeyPath = provenance["key_path"].toString();
            s.provenanceTsaUrl = provenance["tsa_url"].toString();
        }
        const auto updates = parsed["updates"];
        if (updates.isObject()) {
            s.releasesUrl = updates["releases_url"].toString();
            s.dismissedUpdateVersion = updates["dismissed_version"].toString();
            if (updates.hasProperty("auto_download"))
                s.autoDownloadUpdates = static_cast<bool>(updates["auto_download"]);
        }
        const auto diagnostics = parsed["diagnostics"];
        if (diagnostics.isObject()) s.perfTraces = static_cast<bool>(diagnostics["perf_traces"]);
        const auto backup = parsed["backup"];
        if (backup.isObject()) {
            s.scheduledBackup = static_cast<bool>(backup["scheduled"]);
            s.lastScheduledBackupIso = backup["last_success_iso"].toString();
        }
        if (resealWanted && resealPossible) s.save(configDir);
    } else {
        // File exists but is not valid JSON: opt-out defaults are applied
        // (all online ON). Flag it so the UI can warn the user (audit F9).
        s = OnlineSettings{};
        s.source = Source::CorruptFallback;
    }
    return s;
}

OnlineSettings OnlineSettings::update(const juce::File& configDir, const std::function<void(OnlineSettings&)>& fn,
                                     bool* ok)
{
    static std::mutex updateMutex; // read-modify-write is atomic within the app
    const std::lock_guard<std::mutex> lock(updateMutex);
    auto s = load(configDir);
    if (fn) fn(s);
    const bool wrote = s.save(configDir);
    if (ok != nullptr) *ok = wrote;
    return s;
}

bool OnlineSettings::save(const juce::File& configDir) const
{
    configDir.createDirectory();
    const auto file = configDir.getChildFile("settings.json");
    const bool wrote = file.replaceWithText(juce::JSON::toString(toJson(false, configDir)));
    if (wrote) {
        // Best-effort (audit SEC-06): a failure here doesn't undo a
        // successful settings save, it just leaves the file at whatever
        // ACLs it inherited from the config directory.
        FileAcl::restrictToCurrentUser(file.getFullPathName().toStdString());
    }
    return wrote;
}

juce::var OnlineSettings::redactedJson() const
{
    return toJson(true, juce::File{});
}

bool OnlineSettings::saveRedactedTo(const juce::File& destination) const
{
    const auto parent = destination.getParentDirectory();
    if (!parent.createDirectory() && !parent.isDirectory()) return false;
    return destination.replaceWithText(juce::JSON::toString(redactedJson()));
}

juce::var OnlineSettings::toJson(bool redactSecrets, const juce::File& configDir) const
{
    auto* online = new juce::DynamicObject();
    online->setProperty("enabled", enabled);
    online->setProperty("acoustid_lookup", acoustidLookup);
    online->setProperty("llm_copilot", llmCopilot);
    online->setProperty("model_downloads", modelDownloads);
    online->setProperty("hf_hub", hfHub);
    online->setProperty("update_check", updateCheck);
    online->setProperty("lan_listening", lanListening);
    online->setProperty("reference_index", referenceIndex);
    const auto configDirPath = configDirUtf8(configDir);
    auto encodeSecret = [redactSecrets, &configDirPath](const char* slotId, const juce::String& secret) {
        if (redactSecrets) return secret.isNotEmpty() ? juce::String("<redacted>") : juce::String();
        // settings.json keeps only an opaque credential reference whenever the
        // sidecar/native store write succeeds.
        return juce::String(CredentialProtector::protectForSettings(slotId, secret.toStdString(), configDirPath));
    };
    auto* ac = new juce::DynamicObject();
    ac->setProperty("api_key", encodeSecret("acoustid.api_key", acoustidApiKey));
    auto* root = new juce::DynamicObject();
    root->setProperty("online", juce::var(online));
    root->setProperty("acoustid", juce::var(ac));
    auto* llm = new juce::DynamicObject();
    llm->setProperty("provider", llmProvider);
    llm->setProperty("model", llmModel);
    llm->setProperty("anthropic_api_key", encodeSecret("llm.anthropic_api_key", anthropicApiKey));
    llm->setProperty("gemini_api_key", encodeSecret("llm.gemini_api_key", geminiApiKey));
    llm->setProperty("openai_api_key", encodeSecret("llm.openai_api_key", openaiApiKey));
    llm->setProperty("deepseek_api_key", encodeSecret("llm.deepseek_api_key", deepseekApiKey));
    llm->setProperty("moonshot_api_key", encodeSecret("llm.moonshot_api_key", moonshotApiKey));
    llm->setProperty("share_session_context", llmShareSessionContext);
    llm->setProperty("gguf_path", llmGgufPath); // CE-AG-2
    llm->setProperty("local_url", llmLocalUrl);
    root->setProperty("llm", juce::var(llm));
    auto* midi = new juce::DynamicObject();
    midi->setProperty("input_identifier", midiInputIdentifier);
    root->setProperty("midi", juce::var(midi));
    auto* uiPrefs = new juce::DynamicObject();
    uiPrefs->setProperty("session_health_strip", showSessionHealthStrip);
    uiPrefs->setProperty("reduced_motion", reducedMotion); // VIS-7
    uiPrefs->setProperty("ambient_backdrop", ambientBackdrop); // LG-4
    root->setProperty("ui", juce::var(uiPrefs));
    auto* rendering = new juce::DynamicObject();
    rendering->setProperty("use_direct2d", useGpuRendering);
    root->setProperty("rendering", juce::var(rendering));
    auto* compute = new juce::DynamicObject();
    compute->setProperty("accelerator", accelMode);
    root->setProperty("compute", juce::var(compute));
    auto* listening = new juce::DynamicObject(); // NEW-10
    listening->setProperty("monitoring", monitoring);
    root->setProperty("listening", juce::var(listening));
    auto* provenance = new juce::DynamicObject(); // CE-DEL-2
    provenance->setProperty("cert_path", provenanceCertPath);
    provenance->setProperty("key_path", provenanceKeyPath);
    provenance->setProperty("tsa_url", provenanceTsaUrl);
    root->setProperty("provenance", juce::var(provenance));
    auto* backup = new juce::DynamicObject();
    backup->setProperty("scheduled", scheduledBackup);
    backup->setProperty("last_success_iso", lastScheduledBackupIso);
    root->setProperty("backup", juce::var(backup));
    auto* diagnostics = new juce::DynamicObject();
    diagnostics->setProperty("perf_traces", perfTraces);
    root->setProperty("diagnostics", juce::var(diagnostics));
    auto* updates = new juce::DynamicObject();
    updates->setProperty("releases_url", releasesUrl);
    updates->setProperty("dismissed_version", dismissedUpdateVersion);
    updates->setProperty("auto_download", autoDownloadUpdates);
    root->setProperty("updates", juce::var(updates));
    return juce::var(root);
}

} // namespace khensu
