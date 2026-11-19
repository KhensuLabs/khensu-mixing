// desktop-app/Source/session/CloudChatService.h
#pragma once
#include <JuceHeader.h>
#include <optional>
#include "../services/ICloudChat.h"
#include "../ui/shared/CloudChatConsentController.h"

namespace khensu {

class SessionManager;

/// Cloud-chat disclosure and consent (audit DATA-03; ENG-1 stage C4, extracted
/// from AppController). A cloud chat request can include the message/history,
/// BPM, genre, analysis findings, macros, and the current chain - private
/// creative/work-product metadata even when raw audio never leaves the
/// machine. This gates the FIRST cloud-routed chat send behind an explicit,
/// recorded disclosure rather than silently sending it - see
/// docs/data-inventory.md for the full field-level inventory and what's still
/// unimplemented (a dedicated privacy/data-control center, provider-specific
/// links, context minimization toggles).
///
/// Wraps CloudChatConsentController, which needs the session store: the owner
/// calls attach() once khensu.db is open; every ICloudChat call before that is
/// a programming error (asserted, as before). Message thread.
class CloudChatService final : public ICloudChat {
  public:
    CloudChatService() = default;

    /// Called once the session store is open (it persists the consent);
    /// `configDir` holds settings.json (the online settings).
    void attach(SessionManager& store, const juce::File& configDir) { controller_.emplace(store, configDir); }

    /// True if the current online settings route copilot chat to a cloud
    /// provider (Anthropic/Gemini/OpenAI-compatible) rather than the local
    /// heuristic/local-LLM fallback. Re-reads settings.json fresh each call
    /// (cheap) rather than caching, since Settings can change it any time.
    bool isCloudCopilotConfigured() const override;

    /// The configured cloud LLM provider's raw id (e.g. "anthropic"), or
    /// empty if none/local. For the cloud-chat disclosure (audit DATA-03).
    juce::String cloudCopilotProvider() const override;

    /// True once the user has explicitly acknowledged the cloud-chat
    /// disclosure FOR THE CURRENTLY CONFIGURED PROVIDER (audit DATA-03:
    /// consent is per-provider - switching from Anthropic to Gemini re-prompts,
    /// since the data goes to a different company). Persisted in khensu.db's
    /// user_preferences as the consented provider id; a legacy "true" value
    /// (pre-per-provider) deliberately does NOT carry over.
    bool hasCloudChatConsent() const override;

    /// Records that the user acknowledged the disclosure for the currently
    /// configured provider.
    void recordCloudChatConsent() override;

    /// The provider id consent was last granted for ("" = never granted /
    /// revoked). For the Settings consent status line (audit DATA-03).
    juce::String cloudChatConsentedProvider() const override;

    /// Clears the recorded consent - the next cloud-routed chat message
    /// re-prompts (audit DATA-03).
    void revokeCloudChatConsent() override;

    /// Whether cloud-routed chat may include session context (analysis
    /// findings, genre, BPM, current chain) or only the message + history.
    /// Reads settings.json fresh each call (audit DATA-03 context
    /// minimization; local routing ignores this - it never leaves).
    bool shouldShareCloudChatContext() const override;

  private:
    std::optional<CloudChatConsentController> controller_;

    JUCE_DECLARE_NON_COPYABLE(CloudChatService)
};

} // namespace khensu
