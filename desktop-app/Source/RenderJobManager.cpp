// desktop-app/Source/RenderJobManager.cpp
#include "RenderJobManager.h"
#include <khensu/ipc/generated/ApiJobStatus.h>
#include "ui/shared/ServiceErrorHint.h"
#include <algorithm>
#include <thread>
#include <khensu/ipc/generated/ApiSchema.h>

// Service JSON field names, generated from resources/api/openapi.json:
// a renamed Python field is a compile error here, not a silent 0.
namespace apif = khensu::api::fields;

namespace khensu {

namespace {
constexpr int kPollIntervalMs = 750;
// See BatchRenderPanel.cpp for the rationale - a job must not be able to
// poll forever against a dead service with no error budget or timeout.
constexpr int kMaxConsecutivePollErrors = 8;
constexpr juce::int64 kMaxPollErrorElapsedMs = 20000;
} // namespace

std::optional<GuardrailAlert> evaluateGuardrailAlert(const juce::var& fullResult)
{
    const juce::var& g = fullResult[apif::RenderJobResult::guardrail];
    auto* obj = g.getDynamicObject();
    if (obj == nullptr) return std::nullopt;

    const bool guardOk = (bool)obj->getProperty(apif::RenderGuardrail::ok);
    juce::StringArray issues;
    if (auto* blocks = obj->getProperty(apif::RenderGuardrail::blocks).getArray())
        for (const auto& b : *blocks)
            issues.add(b.toString());
    juce::StringArray softWarnings;
    if (auto* warns = obj->getProperty(apif::RenderGuardrail::warnings).getArray())
        for (const auto& w : *warns)
            softWarnings.add(w.toString());
    // The raw artifact-score delta is not a caution by itself: every render
    // with one raised a red "Render caution ... delta: 0.0" banner over the
    // screen (2026-11-18 session check). The service adds a warning above when
    // the delta crosses its threshold; the guardrail summary still lists it.

    if (!guardOk && !issues.isEmpty()) return GuardrailAlert{true, issues.joinIntoString("\n")};
    if (!softWarnings.isEmpty()) return GuardrailAlert{false, softWarnings.joinIntoString("\n")};
    return std::nullopt;
}

std::optional<GuardrailRejection> describeGuardrailRejection(const juce::var& fullResult)
{
    auto* obj = fullResult.getDynamicObject();
    if (obj == nullptr) return std::nullopt;

    const auto quarantinePath = obj->getProperty(apif::RenderJobResult::quarantine_path);
    if (quarantinePath.isVoid() || quarantinePath.toString().isEmpty()) return std::nullopt;

    GuardrailRejection r;
    r.guardrailStatus = obj->getProperty(apif::RenderJobResult::guardrail_status).toString();
    r.sourcePath = obj->getProperty(apif::RenderJobResult::source_path).toString();
    r.quarantinePath = quarantinePath.toString();
    r.outputPath = obj->getProperty(apif::RenderJobResult::output_path).toString();
    r.measurements = obj->getProperty(apif::RenderJobResult::measurements);
    r.serverRenderState = obj->getProperty(apif::RenderJobResult::render_state).toString(); // AI-01 first-class state
    return r;
}

void RenderJobManager::startChainRender(const juce::String& description, const juce::String& filePath,
                                        const juce::String& outputPath,
                                        const std::vector<ipc::HttpServiceBridge::EffectSpec>& chain,
                                        const juce::String& sessionId, const juce::String& sourceRecommendationId,
                                        Callback onDone, bool genreRelative, bool genreActionAllowed,
                                        const juce::String& abstainedReason, const juce::String& targetStem,
                                        const juce::String& decisionHash)
{
    lastChain_ = chain;
    lastChainDecisionHash_ = decisionHash;
    const auto stemPaths = stemProvenanceProvider_ ? stemProvenanceProvider_() : std::map<juce::String, juce::String>{};
    Job job;
    job.info.description = description;
    job.callback = std::move(onDone);
    job.inputPath = filePath;
    job.sessionId = sessionId;
    job.sourceRecommendationId = sourceRecommendationId;
    job.decisionHash = decisionHash;
    registerStart(std::move(job), [filePath, outputPath, chain, sessionId, sourceRecommendationId, stemPaths,
                                   genreRelative, genreActionAllowed, abstainedReason, targetStem,
                                   decisionHash](ipc::HttpServiceBridge& bridge) {
        return bridge.startApplyChainJob(filePath, outputPath, chain, sessionId, sourceRecommendationId, stemPaths,
                                         genreRelative, genreActionAllowed, abstainedReason, targetStem,
                                         decisionHash);
    });
}

void RenderJobManager::startGraphRender(const juce::String& description, const juce::String& filePath,
                                        const juce::String& outputPath, const juce::var& branches,
                                        const juce::var& post, bool normalize, const juce::String& sessionId,
                                        const juce::String& sourceRecommendationId, Callback onDone)
{
    const auto stemPaths = stemProvenanceProvider_ ? stemProvenanceProvider_() : std::map<juce::String, juce::String>{};
    Job job;
    job.info.description = description;
    job.callback = std::move(onDone);
    job.inputPath = filePath;
    job.sessionId = sessionId;
    job.sourceRecommendationId = sourceRecommendationId;
    registerStart(std::move(job), [filePath, outputPath, branches, post, normalize, sessionId, sourceRecommendationId,
                                   stemPaths](ipc::HttpServiceBridge& bridge) {
        return bridge.startApplyGraphJob(filePath, outputPath, branches, post, normalize, sessionId,
                                         sourceRecommendationId, stemPaths);
    });
}

void RenderJobManager::registerStart(
    Job&& job, std::function<khensu::Result<juce::String, ipc::BridgeError>(ipc::HttpServiceBridge&)> startFn)
{
    JUCE_ASSERT_MESSAGE_THREAD

    job.localId = nextLocalId_++;
    job.info.status = "starting";
    job.info.startedMs = juce::Time::currentTimeMillis();
    const auto localId = job.localId;
    jobs_.push_back(std::move(job));
    sendChangeMessage();
    if (!isTimerRunning()) startTimer(kPollIntervalMs);

    juce::WeakReference<RenderJobManager> self(this);
    pool_.addJob([self, localId, startFn = std::move(startFn), baseUrl = baseUrl_]() {
        ipc::HttpServiceBridge bridge(baseUrl);
        auto result = startFn(bridge);
        juce::MessageManager::callAsync([self, localId, result = std::move(result), baseUrl]() mutable {
            if (self == nullptr) return;
            auto* job = [&]() -> Job* {
                for (auto& j : self->jobs_)
                    if (j.localId == localId) return &j;
                return nullptr;
            }();
            if (job == nullptr) {
                // Either genuinely gone (finished/errored already), or the
                // user cancelled while this /start call was still in flight.
                // In the latter case the localId is still in pendingCancels_
                // - cancel server-side now that its jobId has finally arrived.
                auto it = std::find(self->pendingCancels_.begin(), self->pendingCancels_.end(), localId);
                if (it != self->pendingCancels_.end()) {
                    self->pendingCancels_.erase(it);
                    if (result.is_ok()) {
                        const auto jobId = result.value();
                        self->pool_.addJob([jobId, baseUrl]() {
                            ipc::HttpServiceBridge bridge(baseUrl);
                            bridge.cancelApplyChainJob(jobId);
                        });
                    }
                }
                return;
            }
            if (result.is_err()) {
                // One classification for every render (AUDIT-068): the
                // abstention test and the per-status hint live in
                // ServiceErrorHint.h, not in substring checks per caller.
                const auto& error = result.error();
                const juce::String detail = serviceErrorDetail(error);
                const juce::String message =
                    isAbstainedRenderError(error)
                        ? "Render abstained: " + (detail.isNotEmpty() ? detail : juce::String(error.message))
                        : serviceErrorLine(error);
                self->finishJob(localId, false, message, {});
                return;
            }
            job->info.jobId = result.value();
            job->info.status = "pending";
            job->lastPollSuccessMs = juce::Time::currentTimeMillis();
            self->startFollowing(*job);
            self->sendChangeMessage();
        });
    });
}

void RenderJobManager::cancelJob(juce::uint32 localId)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (localId == 0) return;

    const auto it = std::find_if(jobs_.begin(), jobs_.end(), [localId](const Job& j) { return j.localId == localId; });
    if (it == jobs_.end()) return;

    if (it->info.jobId.isNotEmpty()) {
        const juce::String jobId = it->info.jobId;
        finishJob(localId, false, {}, {}); // empty error = user cancel
        pool_.addJob([jobId, baseUrl = baseUrl_]() {
            ipc::HttpServiceBridge bridge(baseUrl);
            bridge.cancelApplyChainJob(jobId); // fire-and-forget
        });
        return;
    }

    // /start hasn't returned yet: fire the cancel callback and drop the job
    // from the visible list now, but remember the localId so the late
    // /start response can be cancelled server-side once its jobId arrives
    // instead of silently rendering unattended.
    pendingCancels_.push_back(localId);
    Callback cb = std::move(it->callback);
    jobs_.erase(it);
    if (jobs_.empty()) stopTimer();
    sendChangeMessage();
    if (cb) cb(false, {}, {});
}

void RenderJobManager::cancelAll()
{
    JUCE_ASSERT_MESSAGE_THREAD
    // Snapshot first: cancelJob() fires callbacks that may submit new jobs.
    std::vector<juce::uint32> ids;
    ids.reserve(jobs_.size());
    for (const auto& j : jobs_)
        ids.push_back(j.localId);
    for (const auto id : ids)
        cancelJob(id);
}

std::vector<RenderJobManager::JobInfo> RenderJobManager::activeJobs() const
{
    std::vector<JobInfo> out;
    out.reserve(jobs_.size());
    for (const auto& j : jobs_) {
        auto info = j.info;
        info.localId = j.localId;
        out.push_back(std::move(info));
    }
    return out;
}

void RenderJobManager::finishJob(juce::uint32 localId, bool ok, const juce::String& outputPathOrError,
                                 const juce::var& fullResult)
{
    JUCE_ASSERT_MESSAGE_THREAD

    Callback cb;
    juce::String inputPath, sessionId, sourceRecommendationId, decisionHash;
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
        if (it->localId == localId) {
            it->stop->store(true); // end its event stream, if any
            cb = std::move(it->callback);
            inputPath = it->inputPath;
            sessionId = it->sessionId;
            sourceRecommendationId = it->sourceRecommendationId;
            decisionHash = it->decisionHash;
            jobs_.erase(it);
            break;
        }
    }
    if (jobs_.empty()) stopTimer();
    sendChangeMessage();

    // Central post-render safety guardrail (2026-07-15 safety audit P0): if the
    // server flagged a matched-level regression (true-peak/clipping/mono-fold),
    // surface it here so NO render flow can silently present a worse result as
    // an improvement, regardless of which panel submitted it.
    if (ok && onRenderCompleted) onRenderCompleted(inputPath, outputPathOrError, fullResult);
    if (ok && onGuardrailAlert) {
        if (const auto alert = evaluateGuardrailAlert(fullResult)) onGuardrailAlert(alert->isBlock, alert->details);
    }
    // A job that failed specifically because the guardrail rejected/errored/
    // was missing (audit AI-01) gets its own callback with the quarantine
    // detail needed for an explicit expert override, distinct from a plain
    // "render failed" message.
    if (!ok && onGuardrailRejected) {
        if (auto rejection = describeGuardrailRejection(fullResult)) {
            rejection->sessionId = sessionId;
            rejection->sourceRecommendationId = sourceRecommendationId;
            rejection->decisionHash = decisionHash;
            onGuardrailRejected(*rejection);
        }
    }

    if (cb) cb(ok, outputPathOrError, fullResult);
}

void RenderJobManager::startFollowing(Job& job)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (job.info.jobId.isEmpty() || job.streaming->load()) return;
    if (activeStreams_->fetch_add(1) >= kMaxStreams) {
        activeStreams_->fetch_sub(1);
        return; // polled instead
    }
    job.streaming->store(true);
    juce::WeakReference<RenderJobManager> self(this);
    pool_.addJob([self, localId = job.localId, jobId = job.info.jobId, streaming = job.streaming, stop = job.stop,
                  active = activeStreams_, baseUrl = baseUrl_]() {
        ipc::HttpServiceBridge bridge(baseUrl);
        bridge.followApplyChainJob(
            jobId,
            [self, localId](const ipc::HttpServiceBridge::RenderJobStatus& s) {
                juce::MessageManager::callAsync([self, localId, s] {
                    if (self != nullptr) self->applyStatus(localId, s);
                });
            },
            [stop] { return stop->load(); });
        // Ended (terminal status delivered, or the stream failed): either the
        // job is finishing or the poll timer picks it up again.
        streaming->store(false);
        active->fetch_sub(1);
    });
}

void RenderJobManager::applyStatus(juce::uint32 localId, const ipc::HttpServiceBridge::RenderJobStatus& s)
{
    JUCE_ASSERT_MESSAGE_THREAD
    auto* job = [&]() -> Job* {
        for (auto& j : jobs_)
            if (j.localId == localId) return &j;
        return nullptr;
    }();
    if (job == nullptr) return; // finished/cancelled meanwhile
    job->consecutivePollErrors = 0;
    job->lastPollSuccessMs = juce::Time::currentTimeMillis();
    if (khensu::api::isActiveStatus(s.status)) {
        if (job->info.status != s.status) {
            job->info.status = s.status;
            sendChangeMessage();
        }
        return;
    }
    if (s.status == khensu::api::job_status::completed)
        finishJob(localId, true, s.outputPath, s.result);
    else if (s.status == khensu::api::job_status::cancelled)
        finishJob(localId, false, {}, {});
    else // "failed" - s.result still carries structured guardrail-rejection
         // data (quarantine_path/guardrail_status/measurements) when that's
         // why it failed (audit AI-01); forwarding it is what lets
         // describeGuardrailRejection() see it.
        finishJob(localId, false, s.error.isNotEmpty() ? s.error : juce::String("Render failed"), s.result);
}

void RenderJobManager::timerCallback()
{
    juce::WeakReference<RenderJobManager> self(this);
    for (auto& j : jobs_) {
        if (j.info.jobId.isEmpty()) continue; // start still in flight
        if (j.streaming->load()) continue;     // its event stream reports it (PERF-6)
        bool expected = false;
        if (!j.pollInFlight->compare_exchange_strong(expected, true)) continue; // previous poll still running

        const auto localId = j.localId;
        const auto jobId = j.info.jobId;
        auto pollInFlight = j.pollInFlight;
        pool_.addJob([self, localId, jobId, pollInFlight, baseUrl = baseUrl_]() {
            ipc::HttpServiceBridge bridge(baseUrl);
            auto result = bridge.getApplyChainJobStatus(jobId);
            juce::MessageManager::callAsync([self, localId, pollInFlight, result = std::move(result)]() mutable {
                pollInFlight->store(false);
                if (self == nullptr) return;
                auto* job = [&]() -> RenderJobManager::Job* {
                    for (auto& j2 : self->jobs_)
                        if (j2.localId == localId) return &j2;
                    return nullptr;
                }();
                if (job == nullptr) return; // finished/cancelled meanwhile
                if (result.is_err()) {
                    ++job->consecutivePollErrors;
                    job->lastPollError = juce::String(result.error().message) +
                                         (result.error().http_status != 0
                                              ? " (HTTP " + juce::String(result.error().http_status) + ")"
                                              : juce::String());
                    const auto elapsed = juce::Time::currentTimeMillis() - job->lastPollSuccessMs;
                    if (job->consecutivePollErrors < kMaxConsecutivePollErrors && elapsed < kMaxPollErrorElapsedMs)
                        return; // transient poll hiccup - keep going

                    // Service loss: stop polling this job and surface it
                    // instead of leaving it "running" forever with no way
                    // to recover (frontend-review-2026-07-17.md P1).
                    // The last error says which: no answer, a timeout, or an HTTP error.
                    self->finishJob(localId, false,
                                    "Analysis service isn't reachable - render status unknown (" +
                                        job->lastPollError + ").",
                                    {});
                    return;
                }
                self->applyStatus(localId, result.value());
            });
        });
    }
}

} // namespace khensu
