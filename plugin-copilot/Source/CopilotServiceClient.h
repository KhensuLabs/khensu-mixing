// plugin-copilot/Source/CopilotServiceClient.h
#pragma once

#include <khensu/ipc/HttpServiceBridge.h>

namespace khensu::copilot {

/// The plugin's one way to reach the desktop-owned service (REF-6). Eight
/// places used to build their own HttpServiceBridge with hand-picked
/// timeouts; they now ask here, and this is where the FIX-7 identity gate is
/// armed, so no request can leave the plugin before it is.
///
/// A bridge is cheap and holds no connection, so each background job takes
/// its own (bridges are not shared across threads). Every request is bounded
/// by its timeout and never retried: a cancel or plugin unload is noticed
/// within one timeout.
struct ServiceClient {
    /// One fire-and-forget call from the short pool (feedback, events, seek,
    /// export). The processor's shutdown wait is sized for it.
    static constexpr int kShortRequestMs = 2500;
    /// One poll or start call of a long service job (analysis, intake, LLM).
    static constexpr int kJobRequestMs = 3000;
    /// The transport-sync thread's calls: it must keep up with the playhead.
    static constexpr int kTransportRequestMs = 1000;

    /// No desktop app may be running to prove who answers on the service
    /// port, so the plugin's bridges prove it themselves (the HMAC /health
    /// challenge) before any request carries the token (FIX-7). Idempotent.
    static void armIdentityGate() noexcept
    {
        ipc::HttpServiceBridge::setServiceIdentityRequired(true);
        ipc::HttpServiceBridge::setServiceIdentitySelfVerify(true);
    }

    /// A bridge for one job, with the identity gate armed.
    static ipc::HttpServiceBridge bridge(int requestTimeoutMs = kShortRequestMs)
    {
        armIdentityGate();
        return ipc::HttpServiceBridge(ipc::HttpServiceBridge::kDefaultBaseUrl, requestTimeoutMs, 0);
    }
};

} // namespace khensu::copilot
