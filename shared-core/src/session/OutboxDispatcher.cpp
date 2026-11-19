#include <khensu/session/OutboxDispatcher.h>

namespace khensu {

namespace {
OutboxDispatcher::DispatchResult deliverEntries(SessionManager& sessionMgr,
                                                const std::vector<SessionManager::OutboxEntry>& entries,
                                                const OutboxDispatcher::SendFn& sendFn)
{
    using Outcome = OutboxDispatcher::SendOutcome;
    OutboxDispatcher::DispatchResult result;
    for (const auto& entry : entries) {
        // The batch was read before any of these sends; "Delete all" may have
        // erased the row since.
        if (!sessionMgr.isOutboxEntryUndelivered(entry.id)) continue;
        switch (sendFn(entry.kind, entry.payloadJson)) {
            case Outcome::Delivered:
                sessionMgr.markOutboxDelivered(entry.id);
                ++result.delivered;
                break;
            case Outcome::RetryLater:
                sessionMgr.markOutboxFailed(entry.id, "delivery attempt failed");
                ++result.failed;
                break;
            case Outcome::Rejected:
                // A poison row (e.g. always 422) used to be retried forever.
                sessionMgr.markOutboxDeadLettered(entry.id, "rejected by the service");
                ++result.deadLettered;
                break;
        }
    }
    return result;
}
} // namespace

OutboxDispatcher::SendOutcome OutboxDispatcher::outcomeForHttpStatus(int httpStatus) noexcept
{
    if (httpStatus >= 200 && httpStatus < 300) return SendOutcome::Delivered;
    // 401: the token rotates on restart; 408/429: transient by definition.
    // CORE-15: 404 is what a STALE service missing /events returns, 409 a
    // conflict that clears, and 403 the "online feature disabled" refusal
    // (the only 403 the feedback/event routes produce) - all recover once the
    // service is updated or the toggle flips, so dead-lettering them threw
    // every queued event away permanently. 503 (shutting down / extra
    // missing) falls through to the 5xx retry below.
    if (httpStatus == 401 || httpStatus == 403 || httpStatus == 404 || httpStatus == 408 || httpStatus == 409 ||
        httpStatus == 429)
        return SendOutcome::RetryLater;
    if (httpStatus >= 400 && httpStatus < 500) return SendOutcome::Rejected;
    return SendOutcome::RetryLater; // 0 (no response), 5xx, anything else
}

OutboxDispatcher::DispatchResult OutboxDispatcher::dispatchDue(SessionManager& sessionMgr, const SendFn& sendFn,
                                                               int limit)
{
    const auto result = deliverEntries(sessionMgr, sessionMgr.dueOutboxEntries(limit), sendFn);
    sessionMgr.pruneDeliveredOutbox(1);
    return result;
}

OutboxDispatcher::DispatchResult OutboxDispatcher::dispatchPending(SessionManager& sessionMgr,
                                                                   const SimpleSendFn& sendFn, int limit)
{
    return deliverEntries(sessionMgr, sessionMgr.pendingOutboxEntries(limit),
                          [&sendFn](const std::string& kind, const std::string& payloadJson) {
                              return sendFn(kind, payloadJson) ? SendOutcome::Delivered : SendOutcome::RetryLater;
                          });
}

} // namespace khensu
