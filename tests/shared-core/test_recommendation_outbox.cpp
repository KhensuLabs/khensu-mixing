// tests/shared-core/test_recommendation_outbox.cpp
// Audit ARCH-03: recommendation-resolution mirroring to the Python
// feedback/provenance stores was a fire-and-forget detached HTTP call with
// no durable record it needed to happen - a service outage or app close
// mid-flight silently dropped the mirror forever. These cover the
// transactional outbox (SessionManager) and its dispatcher: offline apply,
// restart/recovery, and exactly-once convergence.
#include <catch2/catch_test_macros.hpp>
#include <khensu/session/SessionManager.h>
#include <khensu/session/OutboxDispatcher.h>
#include <string>
#include <vector>

using namespace khensu;

TEST_CASE("A freshly enqueued outbox event is pending and undelivered", "[outbox]")
{
    SessionManager mgr(":memory:");
    REQUIRE(mgr.enqueueOutboxEvent("key-1", "feedback", R"({"rec_id":"r1"})").is_ok());

    const auto pending = mgr.pendingOutboxEntries();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].idempotencyKey == "key-1");
    CHECK(pending[0].kind == "feedback");
    CHECK(pending[0].status == "pending");
    CHECK(pending[0].attempts == 0);
    CHECK(mgr.undeliveredOutboxCount() == 1);
}

TEST_CASE("Enqueuing the same idempotency key twice does not create a duplicate row", "[outbox]")
{
    SessionManager mgr(":memory:");
    REQUIRE(mgr.enqueueOutboxEvent("key-1", "feedback", "{}").is_ok());
    REQUIRE(mgr.enqueueOutboxEvent("key-1", "feedback", "{}").is_ok());

    CHECK(mgr.pendingOutboxEntries().size() == 1);
}

TEST_CASE("markOutboxDelivered removes an entry from pendingOutboxEntries and the undelivered count", "[outbox]")
{
    SessionManager mgr(":memory:");
    mgr.enqueueOutboxEvent("key-1", "feedback", "{}");
    const auto id = mgr.pendingOutboxEntries()[0].id;

    REQUIRE(mgr.markOutboxDelivered(id).is_ok());

    CHECK(mgr.pendingOutboxEntries().empty());
    CHECK(mgr.undeliveredOutboxCount() == 0);
}

TEST_CASE("markOutboxFailed keeps the entry pending with an incremented attempt count and recorded error", "[outbox]")
{
    SessionManager mgr(":memory:");
    mgr.enqueueOutboxEvent("key-1", "feedback", "{}");
    const auto id = mgr.pendingOutboxEntries()[0].id;

    REQUIRE(mgr.markOutboxFailed(id, "service unreachable").is_ok());

    const auto pending = mgr.pendingOutboxEntries();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].attempts == 1);
    CHECK(pending[0].status == "failed");
    CHECK(pending[0].lastError == "service unreachable");
    CHECK(mgr.undeliveredOutboxCount() == 1);
}

TEST_CASE("OutboxDispatcher delivers every pending entry when sendFn always succeeds", "[outbox][dispatcher]")
{
    SessionManager mgr(":memory:");
    mgr.enqueueOutboxEvent("key-1", "feedback", "{}");
    mgr.enqueueOutboxEvent("key-2", "event", "{}");

    const auto result = OutboxDispatcher::dispatchPending(mgr, [](const std::string&, const std::string&) {
        return true;
    });

    CHECK(result.delivered == 2);
    CHECK(result.failed == 0);
    CHECK(mgr.undeliveredOutboxCount() == 0);
}

TEST_CASE("A service-offline dispatch leaves entries pending for a later retry (audit's offline-apply case)",
          "[outbox][dispatcher]")
{
    SessionManager mgr(":memory:");
    mgr.enqueueOutboxEvent("key-1", "feedback", "{}");

    const auto offlineResult = OutboxDispatcher::dispatchPending(mgr, [](const std::string&, const std::string&) {
        return false; // service unreachable
    });

    CHECK(offlineResult.delivered == 0);
    CHECK(offlineResult.failed == 1);
    CHECK(mgr.undeliveredOutboxCount() == 1); // still there, not dropped
}

TEST_CASE("Recovery after a failed dispatch converges exactly once (audit's restart/convergence case)",
          "[outbox][dispatcher]")
{
    SessionManager mgr(":memory:");
    mgr.enqueueOutboxEvent("key-1", "feedback", "{}");

    // First attempt: service offline.
    OutboxDispatcher::dispatchPending(mgr, [](const std::string&, const std::string&) { return false; });
    REQUIRE(mgr.undeliveredOutboxCount() == 1);

    // Restart: service back up, retry succeeds.
    const auto recovered = OutboxDispatcher::dispatchPending(mgr, [](const std::string&, const std::string&) {
        return true;
    });
    CHECK(recovered.delivered == 1);
    CHECK(mgr.undeliveredOutboxCount() == 0);

    // A subsequent dispatch pass finds nothing left to (re-)deliver -
    // exactly-once convergence, not a repeat delivery.
    const auto again = OutboxDispatcher::dispatchPending(mgr, [](const std::string&, const std::string&) {
        return true;
    });
    CHECK(again.delivered == 0);
    CHECK(again.failed == 0);
}

TEST_CASE("Mixed batch: one entry fails, one succeeds, in the same dispatch pass", "[outbox][dispatcher]")
{
    SessionManager mgr(":memory:");
    mgr.enqueueOutboxEvent("key-fail", "feedback", "{}");
    mgr.enqueueOutboxEvent("key-ok", "event", "{}");

    const auto result = OutboxDispatcher::dispatchPending(mgr, [](const std::string& kind, const std::string&) {
        return kind == "event"; // only the event delivers
    });

    CHECK(result.delivered == 1);
    CHECK(result.failed == 1);
    CHECK(mgr.undeliveredOutboxCount() == 1);
}

// ---------------------------------------------------------------------------
// AUDIT-056: poison rows, backoff, pruning, atomic batches.
// ---------------------------------------------------------------------------

TEST_CASE("outcomeForHttpStatus dead-letters permanent 4xx but retries transient failures", "[outbox][dispatcher]")
{
    using O = OutboxDispatcher::SendOutcome;
    CHECK(OutboxDispatcher::outcomeForHttpStatus(200) == O::Delivered);
    CHECK(OutboxDispatcher::outcomeForHttpStatus(204) == O::Delivered);
    CHECK(OutboxDispatcher::outcomeForHttpStatus(422) == O::Rejected);
    CHECK(OutboxDispatcher::outcomeForHttpStatus(400) == O::Rejected);
    CHECK(OutboxDispatcher::outcomeForHttpStatus(413) == O::Rejected);
    // CORE-15: a stale service missing the route (404), a transient conflict
    // (409) and the online-disabled refusal (403) all recover - retry them.
    CHECK(OutboxDispatcher::outcomeForHttpStatus(404) == O::RetryLater);
    CHECK(OutboxDispatcher::outcomeForHttpStatus(409) == O::RetryLater);
    CHECK(OutboxDispatcher::outcomeForHttpStatus(403) == O::RetryLater);
    CHECK(OutboxDispatcher::outcomeForHttpStatus(401) == O::RetryLater);
    CHECK(OutboxDispatcher::outcomeForHttpStatus(408) == O::RetryLater);
    CHECK(OutboxDispatcher::outcomeForHttpStatus(429) == O::RetryLater);
    CHECK(OutboxDispatcher::outcomeForHttpStatus(500) == O::RetryLater);
    CHECK(OutboxDispatcher::outcomeForHttpStatus(503) == O::RetryLater);
    CHECK(OutboxDispatcher::outcomeForHttpStatus(0) == O::RetryLater); // no response
}

TEST_CASE("A rejected (poison) entry is dead-lettered once and never sent again", "[outbox][dispatcher]")
{
    SessionManager mgr(":memory:");
    mgr.enqueueOutboxEvent("poison", "event", "{}");
    mgr.enqueueOutboxEvent("good", "event", "{}");

    int sends = 0;
    const auto first = OutboxDispatcher::dispatchDue(mgr, [&sends](const std::string&, const std::string&) {
        ++sends;
        return sends == 1 ? OutboxDispatcher::SendOutcome::Rejected : OutboxDispatcher::SendOutcome::Delivered;
    });
    CHECK(first.deadLettered == 1);
    CHECK(first.delivered == 1);
    CHECK(mgr.undeliveredOutboxCount() == 0); // a dead letter no longer counts as pending work
    CHECK(mgr.deadLetteredOutboxCount() == 1);
    CHECK(mgr.pendingOutboxEntries().empty());

    const auto again = OutboxDispatcher::dispatchDue(mgr, [&sends](const std::string&, const std::string&) {
        ++sends;
        return OutboxDispatcher::SendOutcome::Delivered;
    });
    CHECK(again.delivered + again.failed + again.deadLettered == 0);
    CHECK(sends == 2);
}

TEST_CASE("A failed entry backs off and does not block newer entries behind it", "[outbox][dispatcher]")
{
    SessionManager mgr(":memory:");
    for (int i = 0; i < 3; ++i)
        mgr.enqueueOutboxEvent("old-" + std::to_string(i), "event", "{}");

    // Pass 1 (limit 3): all three fail and enter backoff.
    OutboxDispatcher::dispatchDue(
        mgr, [](const std::string&, const std::string&) { return OutboxDispatcher::SendOutcome::RetryLater; }, 3);
    REQUIRE(mgr.undeliveredOutboxCount() == 3);
    CHECK(mgr.dueOutboxEntries(3).empty()); // still backing off

    // A new entry is picked up even though the batch limit equals the number
    // of backing-off rows ahead of it.
    mgr.enqueueOutboxEvent("new", "event", "{}");
    std::vector<std::string> sentKinds;
    const auto r = OutboxDispatcher::dispatchDue(
        mgr,
        [&sentKinds](const std::string& kind, const std::string&) {
            sentKinds.push_back(kind);
            return OutboxDispatcher::SendOutcome::Delivered;
        },
        3);
    CHECK(r.delivered == 1);
    CHECK(sentKinds.size() == 1);

    // Once the backoff has elapsed the failed rows are due again.
    mgr.execRawSqlForTesting("UPDATE recommendation_outbox SET last_attempt_at = '2000-01-01T00:00:00.000Z' "
                             "WHERE status = 'failed'");
    CHECK(mgr.dueOutboxEntries(10).size() == 3);
}

TEST_CASE("Delivered entries are pruned instead of accumulating forever", "[outbox]")
{
    SessionManager mgr(":memory:");
    mgr.enqueueOutboxEvent("a", "event", "{}");
    mgr.enqueueOutboxEvent("b", "event", "{}");
    OutboxDispatcher::dispatchPending(mgr, [](const std::string&, const std::string&) { return true; });

    CHECK(mgr.pruneDeliveredOutbox(1) == 0); // delivered just now: kept for a day
    mgr.execRawSqlForTesting("UPDATE recommendation_outbox SET last_attempt_at = '2000-01-01T00:00:00.000Z'");
    CHECK(mgr.pruneDeliveredOutbox(1) == 2);
}

TEST_CASE("enqueueOutboxEvents queues a feedback/event pair atomically", "[outbox]")
{
    SessionManager mgr(":memory:");
    REQUIRE(mgr.enqueueOutboxEvents({{"k-feedback", "feedback", "{}"}, {"k-event", "event", "{}"}}).is_ok());
    CHECK(mgr.pendingOutboxEntries().size() == 2);

    // A failure part-way rolls the whole batch back.
    mgr.execRawSqlForTesting("CREATE TRIGGER reject_bad BEFORE INSERT ON recommendation_outbox "
                             "WHEN NEW.kind = 'bad' BEGIN SELECT RAISE(ABORT, 'rejected'); END");
    CHECK_FALSE(mgr.enqueueOutboxEvents({{"x-1", "event", "{}"}, {"x-2", "bad", "{}"}}).is_ok());
    CHECK(mgr.pendingOutboxEntries().size() == 2);
}

TEST_CASE("An entry erased between batch read and send is not posted", "[outbox][dispatcher]")
{
    SessionManager mgr(":memory:");
    mgr.enqueueOutboxEvent("first", "event", "{}");
    mgr.enqueueOutboxEvent("second", "event", "{}");

    int sends = 0;
    OutboxDispatcher::dispatchDue(mgr, [&](const std::string&, const std::string&) {
        ++sends;
        mgr.execRawSqlForTesting("DELETE FROM recommendation_outbox"); // "Delete all" mid-pass
        return OutboxDispatcher::SendOutcome::Delivered;
    });
    CHECK(sends == 1);
}
