#pragma once
#include <khensu/session/SessionManager.h>
#include <functional>
#include <string>

namespace khensu {

/// Drains SessionManager's transactional outbox (audit ARCH-03), delivering
/// each pending entry via an injected send function and marking it
/// delivered/failed based on the result. Decoupled from the actual transport
/// (HttpServiceBridge) via the callback so this can be unit-tested against a
/// real SessionManager(":memory:") without any network involved - the
/// desktop app supplies the real HttpServiceBridge-backed sendFn.
///
/// THREAD_CONTEXT(WorkerThread) - drives SessionManager I/O; callers run this
/// off the message thread the same way the existing fire-and-forget mirror
/// calls did.
class OutboxDispatcher {
  public:
    /// What one delivery attempt achieved (AUDIT-056).
    enum class SendOutcome {
        Delivered,  ///< 2xx - mark delivered
        RetryLater, ///< transport failure, timeout, 401/403/404/408/409/429, 5xx - back off and retry
        Rejected,   ///< any other 4xx (or an unknown kind): permanent, dead-letter it
    };

    /// Classifies an HTTP status from a delivery attempt; 0 = no response.
    static SendOutcome outcomeForHttpStatus(int httpStatus) noexcept;

    /// Given an entry's kind ("feedback" | "event") and payload_json,
    /// attempts delivery and reports the outcome.
    using SendFn = std::function<SendOutcome(const std::string& kind, const std::string& payloadJson)>;
    /// Legacy boolean form: true = delivered, false = retry later.
    using SimpleSendFn = std::function<bool(const std::string& kind, const std::string& payloadJson)>;

    struct DispatchResult {
        int delivered = 0;
        int failed = 0;       ///< left for a later retry
        int deadLettered = 0; ///< permanently rejected
    };

    /// Production path: sends up to `limit` entries whose retry backoff has
    /// elapsed (SessionManager::dueOutboxEntries), oldest first, re-checking
    /// each is still queued right before sending (an erasure mid-pass must
    /// not be re-posted), dead-letters rejected ones, and finally prunes
    /// delivered rows older than a day.
    static DispatchResult dispatchDue(SessionManager& sessionMgr, const SendFn& sendFn, int limit = 200);

    /// Processes up to `limit` not-yet-delivered entries, oldest first,
    /// IGNORING the backoff (a manual "retry now"). A `sendFn` that returns
    /// false leaves the entry pending (attempts++) for a later retry rather
    /// than discarding it - the outbox only drops an entry once it's actually
    /// been delivered.
    static DispatchResult dispatchPending(SessionManager& sessionMgr, const SimpleSendFn& sendFn, int limit = 200);
};

} // namespace khensu
