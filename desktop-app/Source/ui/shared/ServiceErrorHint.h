// desktop-app/Source/ui/shared/ServiceErrorHint.h
// Shared classification of a bridge error into a user-facing message + the
// remedy that actually helps (spec §0). Distinguishes "service not running"
// (Restart) from "endpoint missing / stale build" (Diagnose) from "analysis
// errored" (Retry) so a panel failure tells the user what to DO, not just
// that something broke. Status codes follow local-services common/errors.py
// (one code per failure kind); the FastAPI `detail` the bridge keeps on the
// message separates kinds that share a code (404 file vs route, 503 shutting
// down vs optional dependency missing).
// THREAD_CONTEXT(AnyThread) - pure formatting.
#pragma once
#include <JuceHeader.h>
#include <khensu/ipc/ILocalServiceBridge.h>

namespace khensu {

struct ServiceErrorHint {
    juce::String message; ///< concise, user-facing
    juce::String remedy;  ///< the palette action that helps ("Restart" / "Diagnose" / "Retry")
};

/// The FastAPI `detail` carried on a bridge error. The bridge formats HTTP
/// failures as "HTTP <code> from <path>: <detail>" (or just the detail when a
/// call asked for ErrorDetail::Replace); returns "" when no detail was kept.
inline juce::String serviceErrorDetail(const ipc::BridgeError& e)
{
    const juce::String msg(e.message);
    if (!msg.startsWith("HTTP ")) return msg.trim();
    const int from = msg.indexOf(" from ");
    if (from < 0) return {};
    const int colon = msg.indexOf(from, ": ");
    return colon < 0 ? juce::String() : msg.substring(colon + 2).trim();
}

/// True when a render/agent request was refused because the genre-relative
/// action abstained (no confirmed genre target or reference). The service
/// signals this as a 409 conflict whose detail names the abstention - there
/// is no structured field for it - so the substring test lives here, once,
/// instead of in every caller (audit AUDIT-068).
inline bool isAbstainedRenderError(const ipc::BridgeError& e)
{
    if (e.kind != ipc::BridgeError::Kind::HttpError || e.http_status != 409) return false;
    const juce::String text = serviceErrorDetail(e).isNotEmpty() ? serviceErrorDetail(e) : juce::String(e.message);
    return text.containsIgnoreCase("abstain") || text.containsIgnoreCase("genre-relative")
           || text.containsIgnoreCase("confirm");
}

inline ServiceErrorHint classifyServiceError(const ipc::BridgeError& e)
{
    using Kind = ipc::BridgeError::Kind;
    switch (e.kind) {
    case Kind::ServiceUnavailable:
        return {"The analysis engine isn't reachable.",
                "The notice at the top shows its state; it can be restarted from there or with "
                "\"Restart analysis service\" (Ctrl+K)."};
    case Kind::HttpError: {
        const juce::String detail = serviceErrorDetail(e);
        if (e.http_status == 403) {
            // 403 = forbidden by policy: an online feature switched off in
            // Settings, or an output path outside the allowed write roots.
            if (detail.containsIgnoreCase("online") || detail.containsIgnoreCase("disabled")
                || detail.containsIgnoreCase("settings"))
                return {"This online feature is turned off in Settings.",
                        "Enable it under Settings > Online features, or keep working offline."};
            return {"The service refused to write to that location"
                        + (detail.isNotEmpty() ? ": " + detail : juce::String(".")),
                    "Choose an output folder in your home folder or next to the source file."};
        }
        if (e.http_status == 404) {
            // The service answers 404 both for a missing input file
            // ("<label> not found: <path>") and for an unknown route
            // (FastAPI's bare "Not Found") - only the latter means a stale build.
            const juce::String msg(e.message);
            if (msg.containsIgnoreCase(" not found:"))
                return {"The audio file couldn't be found - it may have been moved, renamed or deleted.",
                        "Relink the session's file, then retry."};
            return {"This analysis endpoint is missing - the running service "
                    "may be an older build.",
                    "Run \"Diagnose analysis service\" (Ctrl+K)."};
        }
        if (e.http_status == 409) {
            if (isAbstainedRenderError(e))
                return {"The render abstained" + (detail.isNotEmpty() ? ": " + detail : juce::String(".")),
                        "Confirm the genre target or add a reference track, then retry."};
            return {"The request conflicts with the current state"
                        + (detail.isNotEmpty() ? ": " + detail : juce::String(".")),
                    "Review the message above, adjust, then retry."};
        }
        if (e.http_status == 413)
            return {"This file is too large for the analysis service's decode budget.",
                    "Trim or downsample the file, then retry."};
        if (e.http_status == 429)
            return {"Too many jobs of this kind are already running.", "Wait for one to finish, then retry."};
        if (e.http_status == 503) {
            if (detail.containsIgnoreCase("shutting down"))
                return {"The analysis service is shutting down.",
                        "Wait for it to restart, or run \"Restart analysis service\" (Ctrl+K)."};
            return {"An optional dependency for this feature isn't installed"
                        + (detail.isNotEmpty() ? ": " + detail : juce::String(".")),
                    "See the service log via \"Diagnose analysis service\"."};
        }
        return {"Analysis failed: " + juce::String(e.message),
                "Retry; if it persists, run \"Diagnose analysis service\"."};
    }
    case Kind::Timeout:
        return {"The analysis service took too long to answer.",
                "Long files can exceed the limit - retry, or run \"Diagnose analysis service\" (Ctrl+K)."};
    case Kind::ParseError:
    default:
        return {"The service returned an unexpected response.", "Run \"Diagnose analysis service\" (Ctrl+K)."};
    }
}

/// Convenience: one-line "message  remedy" for a status label.
inline juce::String serviceErrorLine(const ipc::BridgeError& e)
{
    const auto h = classifyServiceError(e);
    return h.message + "  " + h.remedy;
}

} // namespace khensu
