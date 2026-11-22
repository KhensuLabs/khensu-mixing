#include <khensu/ipc/HttpServiceBridge.h>
#include "HttpServiceBridgeInternal.h"

#include <khensu/common/HmacSha256.h>
#include <khensu/ipc/HttpConnectionLimit.h>
#include <khensu/common/SecureRandom.h>
#include <khensu/ipc/generated/ApiSchema.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace khensu::ipc {

// ---------------------------------------------------------------------------
// Shared deadline thread (audit §8.3). Every bounded body read and every
// CancelWatch registers a callback here instead of starting its own watchdog
// thread. Heap-held behind a shared_ptr (never a static with a joining
// destructor - joining during static destruction / DLL unload is exactly what
// audit L1 removed): shutdownDeadlineScheduler() stops and joins it while JUCE
// is alive, and the next request lazily starts a fresh one.
// ---------------------------------------------------------------------------
namespace {

/// Allocations made in this scope are the deliberate process-lifetime leaks
/// below (CORE-14): the Debug CRT marks them "ignore", so its exit-time dump
/// - which verify_all's debug-capture fails on - reports only real leaks. The
/// leak itself is unchanged; Release builds compile this to nothing. The flag
/// is process-wide, so a concurrent allocation on another thread may be
/// hidden too; that only ever under-reports, and the scopes are short.
class IntentionalLeakScope {
  public:
#if defined(_MSC_VER) && defined(_DEBUG)
    IntentionalLeakScope() : previous_(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG))
    {
        _CrtSetDbgFlag(previous_ & ~_CRTDBG_ALLOC_MEM_DF);
    }
    ~IntentionalLeakScope() { _CrtSetDbgFlag(previous_); }
    IntentionalLeakScope(const IntentionalLeakScope&) = delete;
    IntentionalLeakScope& operator=(const IntentionalLeakScope&) = delete;

  private:
    int previous_;
#else
    IntentionalLeakScope() {} // user-provided: an unused trivial local would warn (C4101, /WX)
#endif
};

class DeadlineScheduler final {
  public:
    using Clock = std::chrono::steady_clock;
    using Id = std::uint64_t;

    DeadlineScheduler() : worker_([this] { run(); }) {}

    /// Runs `onExpire` on the deadline thread at `when` unless disarmed first.
    /// Returns 0 when the scheduler is already stopping (no deadline armed).
    Id arm(Clock::time_point when, std::function<void()> onExpire)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return 0;
        const Id id = nextId_++;
        entries_.emplace(id, Entry{when, std::move(onExpire)});
        byTime_.emplace(when, id);
        wake_.notify_one();
        return id;
    }

    /// Cancels `id`. When its callback is running right now, waits for it to
    /// finish first, so the caller may free whatever the callback touches as
    /// soon as this returns.
    void disarm(Id id)
    {
        if (id == 0) return;
        std::unique_lock<std::mutex> lock(mutex_);
        if (auto it = entries_.find(id); it != entries_.end()) {
            const auto range = byTime_.equal_range(it->second.when);
            for (auto t = range.first; t != range.second; ++t)
                if (t->second == id) {
                    byTime_.erase(t);
                    break;
                }
            entries_.erase(it);
            return;
        }
        fired_.wait(lock, [&] { return firing_ != id; });
    }

    /// Fires nothing further, wakes and joins the thread.
    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    ~DeadlineScheduler() { stop(); }

  private:
    struct Entry {
        Clock::time_point when;
        std::function<void()> fn;
    };

    void run()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_) {
            if (byTime_.empty()) {
                wake_.wait(lock, [this] { return stopping_ || !byTime_.empty(); });
                continue;
            }
            const auto next = byTime_.begin()->first;
            if (Clock::now() < next) {
                wake_.wait_until(lock, next);
                continue;
            }
            const Id id = byTime_.begin()->second;
            byTime_.erase(byTime_.begin());
            auto it = entries_.find(id);
            if (it == entries_.end()) continue;
            auto fn = std::move(it->second.fn);
            entries_.erase(it);
            firing_ = id;
            lock.unlock();
            try {
                if (fn) fn();
            } catch (...) {
                // A deadline callback never takes the shared thread down.
            }
            lock.lock();
            firing_ = 0;
            fired_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable fired_;
    std::map<Id, Entry> entries_;
    std::multimap<Clock::time_point, Id> byTime_;
    Id nextId_ = 1;
    Id firing_ = 0;
    bool stopping_ = false;
    std::thread worker_; // last: started after every other member exists
};

std::mutex& deadlineSchedulerMutex()
{
    static std::mutex mutex;
    return mutex;
}

/// Intentionally leaked holder - see the section comment.
std::shared_ptr<DeadlineScheduler>& deadlineSchedulerSlot()
{
    static auto* slot = [] {
        IntentionalLeakScope leak;
        return new std::shared_ptr<DeadlineScheduler>();
    }();
    return *slot;
}

std::shared_ptr<DeadlineScheduler> deadlineScheduler()
{
    std::lock_guard<std::mutex> lock(deadlineSchedulerMutex());
    auto& slot = deadlineSchedulerSlot();
    if (slot == nullptr) {
        IntentionalLeakScope leak; // lives until exit unless shutdownBackgroundWorkers() stops it
        slot = std::make_shared<DeadlineScheduler>();
    }
    return slot;
}

} // namespace

void shutdownDeadlineScheduler()
{
    std::shared_ptr<DeadlineScheduler> scheduler;
    {
        std::lock_guard<std::mutex> lock(deadlineSchedulerMutex());
        scheduler = std::move(deadlineSchedulerSlot());
    }
    // Joined outside the lock. Readers still holding the old scheduler keep it
    // alive until they disarm; arming on a stopped one returns 0 and they fall
    // back to an inline watchdog for that one read.
    if (scheduler != nullptr) scheduler->stop();
}

juce::String readAllBounded(juce::InputStream& stream, int deadlineMs, bool* timedOut)
{
    if (timedOut != nullptr) *timedOut = false;
    auto* web = dynamic_cast<juce::WebInputStream*>(&stream);
    if (web == nullptr) return stream.readEntireStreamAsString();

    std::atomic<bool> cancelled{false};
    const auto scheduler = deadlineScheduler();
    const auto id = scheduler->arm(DeadlineScheduler::Clock::now() + std::chrono::milliseconds(deadlineMs), [&] {
        cancelled.store(true);
        web->cancel(); // deadline elapsed without the read finishing
    });

    juce::String body;
    if (id != 0) {
        body = web->readEntireStreamAsString();
        // Waits for an in-flight cancel, so `web` / `cancelled` outlive it.
        scheduler->disarm(id);
    } else {
        // The scheduler was shut down between lookup and arm (app/plugin
        // teardown): bound this one read with an inline watchdog instead.
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        std::thread watchdog([&] {
            std::unique_lock<std::mutex> lk(m);
            if (!cv.wait_for(lk, std::chrono::milliseconds(deadlineMs), [&] { return done; })) {
                cancelled.store(true);
                web->cancel();
            }
        });
        body = web->readEntireStreamAsString();
        {
            std::lock_guard<std::mutex> lk(m);
            done = true;
        }
        cv.notify_all();
        watchdog.join();
    }
    // A cancelled read returns a truncated body that would otherwise surface
    // as a misleading "invalid JSON" parse error.
    if (timedOut != nullptr) *timedOut = cancelled.load();
    return body;
}

struct CancelWatch::State {
    std::mutex mutex;
    juce::WebInputStream* web = nullptr;
    std::function<bool()> shouldCancel;
    std::shared_ptr<DeadlineScheduler> scheduler;
    DeadlineScheduler::Id armed = 0;
    int intervalMs = 200;
    bool stopped = false;
    std::atomic<bool> cancelled{false};

    /// Arms the next check; caller holds `mutex`.
    void armNextLocked(const std::shared_ptr<State>& self)
    {
        std::weak_ptr<State> weak = self;
        armed = scheduler->arm(DeadlineScheduler::Clock::now() + std::chrono::milliseconds(intervalMs), [weak] {
            auto st = weak.lock();
            if (st == nullptr) return;
            std::lock_guard<std::mutex> lock(st->mutex);
            if (st->stopped) return;
            if (st->shouldCancel && st->shouldCancel()) {
                st->cancelled.store(true);
                st->web->cancel();
                return;
            }
            st->armNextLocked(st);
        });
    }
};

CancelWatch::CancelWatch(juce::InputStream& stream, std::function<bool()> shouldCancel, int intervalMs)
    : state_(std::make_shared<State>())
{
    auto* web = dynamic_cast<juce::WebInputStream*>(&stream);
    if (web == nullptr || !shouldCancel) return; // nothing that can block, or nothing to watch
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->web = web;
    state_->shouldCancel = std::move(shouldCancel);
    state_->scheduler = deadlineScheduler();
    state_->intervalMs = intervalMs > 0 ? intervalMs : 200;
    state_->armNextLocked(state_);
}

CancelWatch::~CancelWatch()
{
    DeadlineScheduler::Id id = 0;
    std::shared_ptr<DeadlineScheduler> scheduler;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stopped = true;
        id = state_->armed;
        scheduler = state_->scheduler;
    }
    // A check already running re-reads `stopped` under the mutex and returns;
    // disarm() waits for it, so the stream is never touched after this.
    if (scheduler != nullptr) scheduler->disarm(id);
}

bool CancelWatch::cancelled() const noexcept { return state_->cancelled.load(); }

namespace {

// A transport scrub can produce dozens of positions per second. Keep only the
// newest one while a request is in flight, rather than spawning an unbounded
// detached thread for each obsolete position.
class TransportSeekDispatcher final {
  public:
    TransportSeekDispatcher() : worker_([this] { run(); }) {}

    ~TransportSeekDispatcher()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        worker_.join();
    }

    /// Replaces any not-yet-started request with `send` (latest wins).
    void submit(std::function<void()> send)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = std::move(send);
        }
        wake_.notify_one();
    }

  private:
    void run()
    {
        for (;;) {
            std::function<void()> send;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || pending_ != nullptr; });
                if (stopping_) return;
                send = std::move(pending_);
                pending_ = nullptr;
            }
            send();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::function<void()> pending_;
    bool stopping_ = false;
    std::thread worker_;
};

// The dispatcher is created lazily and torn down explicitly by
// HttpServiceBridge::shutdownBackgroundWorkers() while JUCE is still alive
// (audit L1). A function-local static was only joined during static
// destruction, when an in-flight send could still use juce::URL / Logger
// after JUCE had shut down.
//
// The slot is an intentionally leaked heap object, like the deadline
// scheduler's (CORE-14): a seekTransport() after shutdownBackgroundWorkers()
// (a plugin instance created after the last one was destroyed, a late UI
// event) lazily starts a fresh worker, and a function-local static
// unique_ptr would then bring the static-destruction join back. A worker
// still alive at exit is simply never joined (the process ends with it).
std::mutex& transportSeekDispatcherMutex()
{
    static auto* mutex = [] {
        IntentionalLeakScope leak;
        return new std::mutex();
    }();
    return *mutex;
}

std::unique_ptr<TransportSeekDispatcher>& transportSeekDispatcherSlot()
{
    static auto* dispatcher = [] {
        IntentionalLeakScope leak;
        return new std::unique_ptr<TransportSeekDispatcher>();
    }();
    return *dispatcher;
}

void submitTransportSeek(std::function<void()> send)
{
    auto& mutex = transportSeekDispatcherMutex();
    std::lock_guard<std::mutex> lock(mutex);
    auto& dispatcher = transportSeekDispatcherSlot();
    if (dispatcher == nullptr) {
        IntentionalLeakScope leak;
        dispatcher = std::make_unique<TransportSeekDispatcher>();
    }
    dispatcher->submit(std::move(send));
}

} // namespace

namespace {

/// The process-wide token cache behind getHeaders() (see there).
std::string currentToken(bool forceRefresh)
{
    static std::mutex tokenMutex;
    static std::string cachedToken;
    static juce::Time cachedTokenMtime;

    std::lock_guard<std::mutex> lock(tokenMutex);
    const auto mtime = tokenFile().getLastModificationTime();
    if (forceRefresh || cachedToken.empty() || mtime != cachedTokenMtime) {
        cachedToken = readTokenFromDisk().toStdString();
        cachedTokenMtime = mtime;
    }
    return cachedToken;
}

} // namespace

juce::String BridgeTransport::getHeaders(bool forceRefresh) const
{
    // Process-wide token cache: the token is per-launch and shared by every
    // bridge instance, and several call sites construct a fresh bridge per
    // request (e.g. the 5 Hz playhead pollers) - per-instance caching meant a
    // synchronous disk read on every poll.
    //
    // Retries from disk whenever the cached value is empty (not just once,
    // on `!tokenLoaded`): the desktop app's very first bridge call can race
    // the Python service's own startup (it writes token.txt only after it
    // starts). forceRefresh is passed only on the retry after a 401
    // (sendRequest/openStream), so without this an empty first read used to
    // get "loaded" until a request happened to be refused.
    //
    // Also re-read whenever token.txt's modification time changes: the
    // desktop app writes a fresh token on every launch, and a VST3 instance
    // already loaded in a DAW used to keep the old one and 401 every request
    // until the DAW itself restarted. A stat per request is cheap.
    return "Content-Type: application/json\r\nAccept: application/json\r\nAuthorization: Bearer " +
           juce::String(currentToken(forceRefresh));
}



BridgeTransport::BridgeTransport(std::string base_url, int timeout_ms, int retry_delay_ms)
    : base_url_(std::move(base_url)), timeout_ms_(timeout_ms), retry_delay_ms_(retry_delay_ms)
{}

HttpServiceBridge::HttpServiceBridge(std::string base_url, int timeout_ms, int retry_delay_ms)
    : BridgeTransport(std::move(base_url), timeout_ms, retry_delay_ms)
{}



bool HttpServiceBridge::is_service_available() const noexcept
{
    // Through the shared request path (was hand-rolled). /health is
    // unauthenticated, so a 401 retry is meaningless; the body isn't needed.
    BridgeRequestOptions options;
    options.timeoutMs = 1000;
    options.readDeadlineMs = 1000;
    options.parseBody = false;
    options.retryOnUnauthorized = false;
    try {
        return getJson("/health", options).is_ok();
    } catch (...) {
        return false; // noexcept contract: a probe never throws
    }
}

khensu::Result<int, BridgeError>
HttpServiceBridge::sendSessionCredentials(const std::map<std::string, std::string>& credentials)
{
    juce::DynamicObject::Ptr map = new juce::DynamicObject();
    for (const auto& [slot, secret] : credentials)
        map->setProperty(juce::Identifier(juce::String(slot)), juce::String(juce::CharPointer_UTF8(secret.c_str())));
    juce::DynamicObject::Ptr body = new juce::DynamicObject();
    body->setProperty(api::fields::SessionCredentialsRequest::credentials, juce::var(map.get()));
    const auto response = postJsonExpectJson("/settings/credentials", juce::var(body.get()));
    if (response.is_err()) return khensu::Result<int, BridgeError>::err(response.error());
    return khensu::Result<int, BridgeError>::ok(
        static_cast<int>(response.value().getProperty(api::fields::SessionCredentialsResponse::held, 0)));
}

khensu::Result<void, BridgeError> HttpServiceBridge::requestGracefulShutdown()
{
    const auto response = postJsonExpectJson("/shutdown", juce::var(new juce::DynamicObject()));
    if (response.is_err()) return khensu::Result<void, BridgeError>::err(response.error());
    return khensu::Result<void, BridgeError>::ok();
}



namespace {
std::atomic<bool>& identityRequiredFlag()
{
    static std::atomic<bool> flag{false};
    return flag;
}
std::atomic<bool>& identityVerifiedFlag()
{
    static std::atomic<bool> flag{false};
    return flag;
}
std::atomic<bool>& identitySelfVerifyFlag()
{
    static std::atomic<bool> flag{false};
    return flag;
}
/// steady_clock milliseconds of the last successful self-verify proof.
std::atomic<long long>& identityProvedAtMs()
{
    static std::atomic<long long> at{0};
    return at;
}
/// steady_clock milliseconds when the last probe finished, proved or not
/// (0 = none since the gate was last set by hand).
std::atomic<long long>& identityProbeFinishedAtMs()
{
    static std::atomic<long long> at{0};
    return at;
}
std::mutex& identityProbeMutex()
{
    static std::mutex mutex;
    return mutex;
}
long long steadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// AUDIT-020's challenge, for a process with no PythonServiceManager (FIX-7):
/// true only when the listener answers GET /health?nonce=<fresh hex> with
/// HMAC-SHA256(token, nonce). /health is unauthenticated, so the probe
/// carries no token; a listener that cannot read token.txt cannot answer.
bool listenerProvesIdentity(const std::string& baseUrl, int timeoutMs)
{
    const std::string token = currentToken(false);
    if (token.empty()) return false;
    ensureHttpConnectionLimit();
    const juce::String nonce(SecureRandom::hex(32));
    int status = 0;
    juce::URL url(juce::String(baseUrl) + "/health?nonce=" + nonce);
    auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                       .withConnectionTimeoutMs(timeoutMs)
                       .withStatusCode(&status);
    const auto stream = url.createInputStream(options);
    if (stream == nullptr || status != 200) return false;
    const auto json = juce::JSON::parse(stream->readEntireStreamAsString());
    if (!json.isObject()) return false;
    const std::string proof =
        json[khensu::api::fields::HealthResponse::proof].toString().trim().toLowerCase().toStdString();
    return HmacSha256::constantTimeEquals(HmacSha256::hmacHex(token, nonce.toStdString()), proof);
}

/// Self-verify mode: re-prove the listener when there is no proof, or the
/// last one is older than kIdentityProofMaxAgeMs (a service that died could
/// have been replaced on the port since). One probe at a time, and a thread
/// that waited for it uses its answer, proved or not: with the service down,
/// three plugin instances used to probe one after another, so the last one
/// overran its processor's 4 s join. A failed proof also stands for
/// kIdentityRetryAfterFailureMs, so a burst of requests fails fast instead of
/// probing a dead port once each.
void ensureIdentityProved(const std::string& baseUrl, int timeoutMs)
{
    const auto fresh = [] {
        return identityVerifiedFlag().load()
               && steadyNowMs() - identityProvedAtMs().load() < HttpServiceBridge::kIdentityProofMaxAgeMs;
    };
    const auto recentFailure = [] {
        const long long at = identityProbeFinishedAtMs().load();
        return at != 0 && !identityVerifiedFlag().load()
               && steadyNowMs() - at < HttpServiceBridge::kIdentityRetryAfterFailureMs;
    };
    if (fresh() || recentFailure()) return;
    const long long waitedFrom = steadyNowMs();
    const std::lock_guard<std::mutex> lock(identityProbeMutex());
    if (fresh() || recentFailure() || identityProbeFinishedAtMs().load() >= waitedFrom) return;
    // /health answers in milliseconds; a busy service can take over a second
    // (CLAUDE.md: the desktop probe allows 2.5 s).
    const bool proved = listenerProvesIdentity(baseUrl, juce::jlimit(250, 2500, timeoutMs));
    const long long now = steadyNowMs();
    if (proved) identityProvedAtMs().store(now);
    identityVerifiedFlag().store(proved);
    identityProbeFinishedAtMs().store(now);
}
} // namespace

void HttpServiceBridge::setServiceIdentityRequired(bool required) noexcept { identityRequiredFlag().store(required); }
void HttpServiceBridge::setServiceIdentityVerified(bool verified) noexcept
{
    // Set by hand (the desktop's health probe, a test): forget the last
    // self-verify answer so it cannot stand in for this one.
    identityProbeFinishedAtMs().store(0);
    identityVerifiedFlag().store(verified);
}
bool HttpServiceBridge::serviceIdentityVerified() noexcept { return identityVerifiedFlag().load(); }
bool HttpServiceBridge::serviceIdentityRequired() noexcept { return identityRequiredFlag().load(); }
void HttpServiceBridge::setServiceIdentitySelfVerify(bool selfVerify) noexcept
{
    identitySelfVerifyFlag().store(selfVerify);
}
bool HttpServiceBridge::serviceIdentitySelfVerify() noexcept { return identitySelfVerifyFlag().load(); }

void HttpServiceBridge::shutdownBackgroundWorkers()
{
    std::unique_ptr<TransportSeekDispatcher> dispatcher;
    {
        auto& mutex = transportSeekDispatcherMutex();
        std::lock_guard<std::mutex> lock(mutex);
        dispatcher = std::move(transportSeekDispatcherSlot());
    }
    // Joined outside the lock: an in-flight send is bounded by its 1.5 s
    // timeout; a queued-but-unsent seek is dropped.
    dispatcher.reset();
    // The shared read-deadline thread (audit §8.3) - same contract.
    shutdownDeadlineScheduler();
}



void HttpServiceBridge::seekTransport(float position_sec)
{
    // Latest-wins worker (a scrub emits dozens of positions a second); each
    // send goes through the shared helper, so it gets the token re-read and
    // 401 retry, the read deadline, and a typed error for the log.
    const juce::String path = "/transport/seek?pos=" + juce::String(position_sec, 4);
    submitTransportSeek([baseUrl = base_url_, path]() {
        const HttpServiceBridge bridge(baseUrl, 1500, 0);
        BridgeRequestOptions options;
        options.readDeadlineMs = 1500;
        options.parseBody = false;
        const auto result = bridge.postJson(path, juce::var(new juce::DynamicObject()), options);
        if (result.is_err())
            juce::Logger::writeToLog("seekTransport failed: " + juce::String(result.error().message));
    });
}



bool BridgeTransport::serviceIsListening() const
{
    const juce::URL base{juce::String(base_url_)};
    const int port = base.getPort() > 0 ? base.getPort() : 80;
    juce::StreamingSocket probe;
    // A local listener accepts (into its backlog) immediately, even while its
    // workers are busy; 250 ms is ample on localhost.
    const bool connected = probe.connect(base.getDomain(), port, 250);
    probe.close();
    return connected;
}

std::unique_ptr<juce::InputStream> BridgeTransport::openStream(const char* method, const juce::String& pathAndQuery,
                                                                 const juce::String* postBody,
                                                                 const BridgeRequestOptions& options,
                                                                 BridgeError& error) const
{
    ensureHttpConnectionLimit(); // WinINet's default is 2 per server
    // Error messages name the route, not its query string or the base URL.
    const juce::String path = pathAndQuery.upToFirstOccurrenceOf("?", false, false);
    const std::string pathStd = path.toStdString();
    const auto unreachable = [&] {
        if (!options.unreachableMessage.empty()) return options.unreachableMessage;
        return (options.non200IsUnavailable ? "Could not poll " : "Could not reach ") + pathStd;
    };

    // /health is unauthenticated server-side and is the one route allowed
    // before the listener has proved its identity, so it never carries the
    // token: otherwise any process squatting on the port would collect it
    // from the first availability probe.
    const bool unauthenticatedRoute = path == "/health";

    // AUDIT-020: never hand the token to an unverified listener. In
    // self-verify mode (the plugin, FIX-7) prove it here first.
    if (identityRequiredFlag().load() && !unauthenticatedRoute) {
        if (identitySelfVerifyFlag().load())
            ensureIdentityProved(base_url_, options.timeoutMs > 0 ? options.timeoutMs : timeout_ms_);
        if (!identityVerifiedFlag().load()) {
            error = {BridgeError::Kind::ServiceUnavailable, unreachable() + " (service identity not verified)"};
            return nullptr;
        }
    }

    juce::URL url(juce::String(base_url_) + pathAndQuery);
    if (postBody != nullptr) url = url.withPOSTData(*postBody);
    // inPostData moves URL query parameters INTO the body ("pos=1.2{}"), which
    // broke query-parameter POST routes such as /transport/seek?pos=. Keep the
    // query in the address when there is one; JUCE still sends the POST body
    // (and uses POST) because withPOSTData() set body data.
    const bool hasQuery = pathAndQuery.containsChar('?');
    const auto handling = (postBody != nullptr && !hasQuery) ? juce::URL::ParameterHandling::inPostData
                                                             : juce::URL::ParameterHandling::inAddress;
    const int timeoutMs = options.timeoutMs > 0 ? options.timeoutMs : timeout_ms_;

    int statusCode = 0;
    std::unique_ptr<juce::InputStream> stream;
    double attemptStartMs = 0.0;
    // The token rotates on every service launch; getHeaders() already re-reads
    // it when token.txt's mtime changes, but a write racing the stat (or a
    // coarse mtime) can still leave a stale token - a 401 forces one re-read
    // and a single retry before reporting the failure.
    for (int attempt = 0; attempt < 2; ++attempt) {
        statusCode = 0;
        attemptStartMs = juce::Time::getMillisecondCounterHiRes();
        const auto streamOptions = juce::URL::InputStreamOptions(handling)
                                       .withConnectionTimeoutMs(timeoutMs)
                                       .withStatusCode(&statusCode)
                                       .withExtraHeaders(unauthenticatedRoute
                                                             ? juce::String("Accept: application/json")
                                                             : getHeaders(attempt > 0));
        // (InputStreamOptions isn't assignable - its handling member is const.)
        const auto withCancel = options.shouldCancel
                                    ? streamOptions.withProgressCallback(
                                          [cancel = options.shouldCancel](int, int) { return !cancel(); })
                                    : streamOptions;
        stream = url.createInputStream(method != nullptr ? withCancel.withHttpRequestCmd(method) : withCancel);
        if (stream == nullptr && options.shouldCancel && options.shouldCancel()) {
            error = {BridgeError::Kind::ServiceUnavailable, "Cancelled: " + pathStd};
            return nullptr;
        }
        if (stream == nullptr || statusCode != 401 || !options.retryOnUnauthorized) break;
    }

    if (stream == nullptr) {
        // CORE-03: JUCE applies timeoutMs as WinINet's connect AND receive
        // timeout, and FastAPI sends no headers until a synchronous handler
        // returns - so a null stream after (nearly) the whole timeout is a
        // SLOW service that is still computing, not an unreachable one. Time
        // alone can't tell them apart (Windows retries a refused localhost
        // connect for ~2 s), so once the time is (nearly) up, a quick TCP
        // probe decides: something is listening -> it just didn't answer.
        const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - attemptStartMs;
        if (elapsedMs >= static_cast<double>(timeoutMs) * 0.9 && serviceIsListening()) {
            error = {BridgeError::Kind::Timeout,
                     "Timed out after " + std::to_string((timeoutMs + 500) / 1000) + " s waiting for " + pathStd +
                         " - the service may still be working on it"};
            return nullptr;
        }
        error = {BridgeError::Kind::ServiceUnavailable, unreachable()};
        return nullptr;
    }
    if (statusCode == 401) {
        // CORE-14: a 401 that survived the token re-read is an auth failure
        // (stale token.txt, a different service on the port) - reporting it
        // as "Could not poll" sent users looking for a dead service.
        error = {BridgeError::Kind::HttpError,
                 "HTTP 401 from " + pathStd + ": the service rejected the access token (token.txt is stale or "
                                              "belongs to another service instance)",
                 401};
        return nullptr;
    }
    if (options.non200IsUnavailable && statusCode != 200) {
        error = {BridgeError::Kind::ServiceUnavailable, unreachable(), statusCode};
        return nullptr;
    }
    if (statusCode < 200 || statusCode >= 300) {
        std::string message = "HTTP " + std::to_string(statusCode) + " from " + pathStd;
        // 403 / 404 / 409 / 413 / 422 / 429 / 503 detail is always kept: it is
        // what tells "missing file" apart from "missing route" (FastAPI's
        // unknown-route 404 says just "Not Found"), "shutting down" apart from
        // "optional dependency missing" (both 503), and names the rejected
        // input or the abstention for the user (ui/shared/ServiceErrorHint.h).
        auto detailMode = options.errorDetail;
        if (detailMode == BridgeRequestOptions::ErrorDetail::Ignore
            && (statusCode == 403 || statusCode == 404 || statusCode == 409 || statusCode == 413
                || statusCode == 422 || statusCode == 429 || statusCode == 503))
            detailMode = BridgeRequestOptions::ErrorDetail::Append;
        if (detailMode != BridgeRequestOptions::ErrorDetail::Ignore) {
            // FastAPI reports the reason as {"detail": ...}.
            const int readDeadlineMs = options.readDeadlineMs > 0 ? options.readDeadlineMs : kReadDeadlineMs;
            const auto errorJson = juce::JSON::parse(readAllBounded(*stream, readDeadlineMs));
            juce::String detail;
            if (auto* obj = errorJson.getDynamicObject()) {
                const auto detailVar = obj->getProperty("detail");
                if (!detailVar.isVoid() && !detailVar.isUndefined()) detail = detailVar.toString();
            }
            if (detail.isNotEmpty())
                message = detailMode == BridgeRequestOptions::ErrorDetail::Replace
                              ? detail.toStdString()
                              : message + ": " + detail.toStdString();
        }
        error = {BridgeError::Kind::HttpError, message, statusCode};
        return nullptr;
    }
    return stream;
}



BridgeTransport::JsonResult BridgeTransport::sendRequest(const char* method, const juce::String& pathAndQuery,
                                                             const juce::String* postBody,
                                                             const BridgeRequestOptions& options) const
{
    BridgeError error;
    auto stream = openStream(method, pathAndQuery, postBody, options, error);
    if (stream == nullptr) return JsonResult::err(std::move(error));
    if (!options.parseBody) return JsonResult::ok(juce::var());

    const std::string pathStd = pathAndQuery.upToFirstOccurrenceOf("?", false, false).toStdString();
    const int readDeadlineMs = options.readDeadlineMs > 0 ? options.readDeadlineMs : kReadDeadlineMs;
    bool timedOut = false;
    const auto body = readAllBounded(*stream, readDeadlineMs, &timedOut);
    // A cancelled read leaves a truncated body that would otherwise surface
    // as a misleading parse error.
    if (timedOut)
        return JsonResult::err({BridgeError::Kind::Timeout, "Timed out reading the response from " + pathStd});
    auto json = juce::JSON::parse(body);
    if (json.isVoid() || (options.requireObject && !json.isObject()))
        return JsonResult::err({BridgeError::Kind::ParseError, "Invalid JSON from " + pathStd});
    return JsonResult::ok(std::move(json));
}



BridgeTransport::JsonResult BridgeTransport::postJson(const juce::String& path, const juce::var& body,
                                                          const BridgeRequestOptions& options) const
{
    const auto text = juce::JSON::toString(body);
    return sendRequest(nullptr, path, &text, options);
}

BridgeTransport::JsonResult BridgeTransport::postText(const juce::String& path, const juce::String& body,
                                                          const BridgeRequestOptions& options) const
{
    return sendRequest(nullptr, path, &body, options);
}

BridgeTransport::JsonResult BridgeTransport::getJson(const juce::String& pathAndQuery,
                                                         const BridgeRequestOptions& options) const
{
    return sendRequest(nullptr, pathAndQuery, nullptr, options);
}

bool BridgeTransport::sendDelete(const juce::String& path) const
{
    BridgeRequestOptions options;
    options.parseBody = false;
    return sendRequest("DELETE", path, nullptr, options).is_ok();
}

void BridgeTransport::postIgnoringResult(const juce::String& path) const
{
    BridgeRequestOptions options;
    options.timeoutMs = 2000;
    options.parseBody = false;
    const juce::String empty;
    (void)sendRequest(nullptr, path, &empty, options); // result is ignored either way
}

BridgeTransport::JsonResult BridgeTransport::postJsonExpectJson(const juce::String& path,
                                                                    const juce::var& body) const
{
    // Long render/encode callers construct the bridge with a larger explicit
    // timeout. All other callers inherit the short constructor default.
    BridgeRequestOptions options;
    options.timeoutMs = juce::jlimit(1000, 300000, timeout_ms_);
    options.readDeadlineMs = options.timeoutMs;
    return postJson(path, body, options);
}



} // namespace khensu::ipc
