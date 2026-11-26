// desktop-app/Source/analysis/AnalysisController.cpp
#include "AnalysisController.h"
#include "../ui/shared/BackgroundStop.h"
#include "../ui/shared/GenreInUse.h"
#include "DeepAnalysisJobRunner.h"
#include "ServiceReadyWait.h"
#include <khensu/analysis/AnalysisInputs.h>
#include <khensu/analysis/AudioAnalyzer.h>
#include <khensu/analysis/BatchResultAggregator.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/targets/GenreDatabase.h>
#include <khensu/targets/UserPreferenceModel.h>
#include <future>
#include <optional>

namespace khensu {

// Copies every deep-analysis field off a DeepAnalysisResponse onto an
// AnalysisResult. Previously duplicated verbatim in three places (preloaded,
// single-file job-poll, batch blocking) - one helper keeps them in lockstep.
static void applyDeepResponse(AnalysisResult& result, const ipc::DeepAnalysisResponse& deep)
{
    deep.apply_to(result.python_data);
    result.python_analysis_ran = true;
    result.mono_compat = deep.mono_compat;
    result.translation_risk = deep.translation_risk;
    result.harshness = deep.harshness;
    result.resonance = deep.resonance;
    result.mid_side_imbalance = deep.mid_side_imbalance;
    result.emotional_arc = deep.emotional_arc;
    result.genre_blend = deep.genre_blend;
    result.genre_target_source = deep.genre_target_source;
    result.genre_confirmation_status = deep.genre_confirmation_status;
    result.genre_abstained_reason = deep.genre_abstained_reason;
    result.genre_relative_action_allowed = deep.genre_relative_action_allowed;
    result.genre_is_hybrid = deep.genre_is_hybrid;
    result.translation_prediction = deep.translation_prediction;
    result.benchmark = deep.benchmark;
    result.stereo_field_timeline = deep.stereo_field_timeline;
}

AnalysisServiceCalls AnalysisServiceCalls::localService(std::shared_ptr<const service_ready::EngineGate> engine)
{
    AnalysisServiceCalls calls;
    // The lifecycle's verdict when it has one (OVH-1); otherwise the probe,
    // which keeps one bridge. Every other request builds its own, so the
    // five that run together share nothing.
    calls.available = [engine, bridge = std::make_shared<ipc::HttpServiceBridge>()] {
        return service_ready::engineAvailable(engine.get(), [&bridge] { return bridge->is_service_available(); });
    };
    calls.knownNotReady = [engine] { return service_ready::engineKnownNotReady(engine.get()); };
    calls.deep = [](const std::string& path, const DeepAnalysisJobHooks& hooks) {
        ipc::HttpServiceBridge bridge;
        return runDeepAnalysisJob(bridge, path, hooks);
    };
    calls.sections = [](const std::string& path) { return ipc::HttpServiceBridge{}.analyzeSections(path); };
    calls.drums = [](const std::string& path) { return ipc::HttpServiceBridge{}.analyzeDrums(path); };
    calls.reference = [](const std::string& path, const std::string& reference) {
        return ipc::HttpServiceBridge{}.analyzeReference(path, reference);
    };
    calls.chords = [](const std::string& path) { return ipc::HttpServiceBridge{}.analyzeChords(path); };
    calls.trust = [](const std::string& path) { return ipc::HttpServiceBridge{}.analyzeMeasurementQuality(path); };
    return calls;
}

AnalysisServiceCalls AnalysisServiceCalls::fixCheck(std::shared_ptr<const service_ready::EngineGate> engine)
{
    auto calls = localService(std::move(engine));
    calls.deep = [](const std::string& path, const DeepAnalysisJobHooks& hooks) {
        ipc::HttpServiceBridge bridge;
        DeepAnalysisJobHooks fixCheckHooks = hooks;
        fixCheckHooks.mode = ipc::ILocalServiceBridge::AnalysisMode::FixCheck;
        return runDeepAnalysisJob(bridge, path, fixCheckHooks);
    };
    calls.chords = nullptr;    // no finding is built from chords
    calls.reference = nullptr; // nor from the reference deltas
    calls.scansWithDeep = true;
    return calls;
}

AnalysisController::AnalysisController(const GenreDatabase& genres, FeedbackManager* feedbackMgr,
                                       AnalysisServiceCalls calls)
    : sharedState_(std::make_shared<SharedState>()), genres_(genres), feedbackMgr_(feedbackMgr),
      service_(std::make_shared<const AnalysisServiceCalls>(std::move(calls)))
{
    sharedState_->owner = this;
}

AnalysisController::~AnalysisController()
{
    {
        std::lock_guard<std::mutex> lock(sharedState_->mutex);
        sharedState_->owner = nullptr;
    }
    cancel();
    // Non-blocking removal: do not wait for a request already on the wire.
    // A job sees `owner == nullptr` (superseded(), below) before the deep job,
    // on each of its polls and before the follow-up requests, and returns; it
    // touches only what it captured by value or shares (state, service), so
    // it may finish after this object is gone. A request already sent is not
    // aborted - it ends with its own timeout (FIX-5).
    // Was removeAllJobs(true, -1), which blocked the message thread up to 60 s.
    pool_.removeAllJobs(false, 0);
}

void AnalysisController::cancel()
{
    sharedState_->generation.fetch_add(1); // invalidate any in-flight result

    // Tell the service to actually stop the remote job instead of only
    // dropping the local result on the floor (CQ-04): a cancelled analysis
    // previously kept consuming the service's one worker slot until its own
    // 120s poll ceiling elapsed, delaying the next analysis behind it.
    std::string jobId;
    {
        std::lock_guard<std::mutex> lock(sharedState_->mutex);
        jobId = sharedState_->currentJobId;
        sharedState_->currentJobId.clear();
        sharedState_->currentJobGeneration = -1;
    }
    if (!jobId.empty()) {
        // Fire-and-forget on its own thread: cancel() runs on the UI thread
        // and must not block on the POST's connection timeout.
        cancelPool_.addJob([jobId] {
            ipc::HttpServiceBridge bridge;
            bridge.cancelAnalysisJob(jobId);
        });
    }
}

void AnalysisController::clear()
{
    cancel();
    if (hasResult_ && current_.has_python_analysis() && !cachedSessionId_.empty())
        reopenCache_ = current_;
    current_ = AnalysisResult{};
    hasResult_ = false;
}

void AnalysisController::analyzeSession(const Session& session, ResultCallback onUpdate, ProgressCallback onProgress,
                                       bool forceRerun, bool keepCurrentResult)
{
    if (session.files.empty()) return;

    // Re-opening the same session: deliver the cached result without re-running,
    // but only if the audio file hasn't been modified on disk since last analysis.
    // Two guards force a fresh run instead of serving the cache:
    //  - `forceRerun` (explicit recovery re-run when the service comes back), and
    //  - a cached result that is C++-only (no Python/deep half): it was produced
    //    while the service was down, so re-opening the session should try the
    //    deep pass again rather than re-serve the stale partial. Re-running is
    //    cheap when the service is still down (fast C++ + an instant deep-skip).
    const auto file = session.files.front();
    const juce::int64 currentMtime = juce::File(juce::String(file.path)).getLastModificationTime().toMilliseconds();
    if (!forceRerun && hasResult_ && current_.has_python_analysis() && !cachedSessionId_.empty() &&
        cachedSessionId_ == session.id && cachedFileMtime_ == currentMtime) {
        if (onUpdate) onUpdate(current_);
        return;
    }
    // The same session reopened after clear() (a session switch in between).
    if (!forceRerun && reopenCache_.has_value() && cachedSessionId_ == session.id && cachedFileMtime_ == currentMtime) {
        current_ = *reopenCache_;
        hasResult_ = true;
        reopenCache_.reset();
        sharedState_->generation.fetch_add(1); // nothing older may land on top
        if (onUpdate) onUpdate(current_);
        return;
    }
    reopenCache_.reset();

    // A fresh run: forget the previous result. currentResult() is read by pull
    // (Copilot chat, System Brain, screens on visibility); leaving the old
    // value would show the previous session's numbers until this run lands.
    // An in-place re-run of the same session (FN2-2b: the native result on
    // screen gains its deep half) keeps it until the new one replaces it.
    if (!keepCurrentResult) {
        current_ = AnalysisResult{};
        hasResult_ = false;
        cachedSessionId_.clear();
    }

    // This single increment both cancels any in-flight run and claims the new generation.
    const int myGen = sharedState_->generation.fetch_add(1) + 1; // claim a new generation
    // ...but only locally: the superseded run's remote deep job kept the
    // service's single worker busy until its own deadline, queueing this run
    // behind it. Cancel it server-side too (same path as cancel()).
    {
        std::string staleJobId;
        {
            std::lock_guard<std::mutex> lock(sharedState_->mutex);
            staleJobId = sharedState_->currentJobId;
            sharedState_->currentJobId.clear();
            sharedState_->currentJobGeneration = -1;
        }
        if (!staleJobId.empty())
            cancelPool_.addJob([staleJobId] {
                ipc::HttpServiceBridge bridge;
                bridge.cancelAnalysisJob(staleJobId);
            });
    }
    const std::string sessionId = session.id;
    const std::string genreId = session.target.primary_genre;
    const CreativeTarget target = session.target;

    GenreEntry genre;
    if (auto g = genres_.findById(genreId)) genre = *g;

    FeedbackManager* localFeedbackMgr = feedbackMgr_;

    // Consume the one-shot intake cache when it matches this session's first
    // file (path + mtime) - avoids re-running the 15-60 s deep pass that
    // Session Intake already paid for during auto-populate.
    // Consume per-file intake cache entries when path+mtime match - avoids
    // re-running the 15-60 s Python pass that Session Intake already paid for.
    std::map<std::string, PreloadedDeep> preloadedDeep;
    for (auto& [path, entry] : preloadedDeep_) {
        const juce::int64 mtime = juce::File(juce::String(path)).getLastModificationTime().toMilliseconds();
        if (entry.mtime == mtime) preloadedDeep[path] = {entry.mtime, std::move(entry.response)};
    }
    preloadedDeep_.clear();

    pool_.addJob([state = sharedState_, myGen, session, sessionId, genre, genreId, target, onUpdate, onProgress,
                  storedNative = storedNative_,
                  service = service_, currentMtime, localFeedbackMgr, &genres = genres_,
                  preloadedDeep = std::move(preloadedDeep), localRefPath = referenceFilePath_]() mutable {
        // Any uncaught exception here previously killed the ThreadPool job
        // silently - the final MessageManager::callAsync below never ran, so
        // the UI (sidebar "Analyzing..." status) waited forever with no error.
        // Every exit path must now reach the dispatch-or-fail logic at the
        // bottom of this lambda.
        auto dispatchFailure = [state, myGen, onUpdate](const juce::String& message) {
            AnalysisResult failed;
            failed.is_partial = false;
            failed.analysis_failed = true;
            failed.error_message = message.toStdString();
            juce::MessageManager::callAsync([state, myGen, failed, onUpdate] {
                if (!state->mayDeliver(myGen)) return;
                // Note: does not touch current_/hasResult_ - a failed run
                // should not poison the cache used to skip re-analysis.
                if (onUpdate) onUpdate(failed);
            });
        };

        auto reportProgress = [state, myGen, onProgress](const std::string& stage, int idx, int total) {
            if (!onProgress) return;
            juce::MessageManager::callAsync([state, myGen, onProgress, stage, idx, total] {
                if (!state->mayDeliver(myGen)) return;
                onProgress(stage, idx, total);
            });
        };

        // True once this run is not the one anybody is waiting for: cancelled,
        // replaced by a newer run, the controller destroyed, or the app
        // shutting down. Nothing it produced can be delivered (mayDeliver),
        // so it stops asking the engine for more (FIX-5).
        const auto superseded = [state, myGen] {
            if (backgroundShouldStop()) return true;
            std::lock_guard<std::mutex> lock(state->mutex);
            return state->owner == nullptr || state->generation.load() != myGen;
        };

        try {
            std::vector<AnalysisResult> batchResults;
            // A multitrack session grades its mixdown only (WF-7).
            const auto analysedFiles = session.analysedFiles();

            for (const auto& file : analysedFiles) {
                if (superseded()) return;
                // ---- Fast analysis (WorkerThread) ----
                // PERF-3: a reopened session reuses its stored native pass
                // when the file is unchanged; the native pass was nearly
                // all of a reopen's time (the service answers from its cache).
                std::optional<AnalysisResult> reused;
                if (storedNative != nullptr && *storedNative && analysedFiles.size() == 1)
                    reused = (*storedNative)(sessionId, file);
                AnalysisResult result;
                if (reused.has_value()) {
                    result = std::move(*reused);
                    result.id = juce::Uuid().toString().toStdString();
                    result.session_id = sessionId;
                    result.file_id = file.id;
                    result.computed_at = juce::Time::getCurrentTime().toISO8601(true).toStdString();
                    juce::Logger::writeToLog("AnalysisController: reused the stored native pass for " +
                                             juce::String(file.path));
                } else {
                    AudioAnalyzer analyzer;
                    auto fastRes = analyzer.analyze(file, sessionId);
                    if (!fastRes.is_ok()) continue;
                    result = fastRes.value();
                }
                result.is_partial = true;

                // Generate recommendations from fast C++ data
                RecommendationEngine engine;
                result.recommendations = engine.generate(result, genre, target);

                // Deliver the fast result immediately so the UI can render within
                // seconds and show "Deep analysis..." while Python works (15-60 s).
                // Single-file sessions only - batch partials would flicker as each
                // file's fast pass lands.
                if (analysedFiles.size() == 1) {
                    AnalysisResult partial = result;
                    juce::MessageManager::callAsync([state, myGen, partial, onUpdate] {
                        if (!state->mayDeliver(myGen)) return;
                        if (onUpdate) onUpdate(partial);
                    });
                    reportProgress("fast", 1, 6);
                }

                // ---- Deep analysis (WorkerThread, optional) ----
                const bool singleFile = analysedFiles.size() == 1;
                // The Intake "Enable AI Deep Analysis" toggle (target.run_deep_analysis)
                // was persisted but never consulted; honour it here.
                // Wait for the engine rather than one 1 s probe: a busy cold
                // start missed it and silently skipped the deep pass
                // (ServiceReadyWait.h). Cancelled with the analysis.
                bool engineReady = false;
                if (target.run_deep_analysis && service->knownNotReady && service->knownNotReady()) {
                    // FN2-2b: the lifecycle says the engine is starting (a
                    // first launch's cold start) or down. Waiting would only
                    // hold the native findings back: this result is complete
                    // without the engine's inputs, which are marked
                    // unanswered, and AnalysisService re-runs it in place
                    // once the engine is Running.
                    juce::Logger::writeToLog("AnalysisController: analysis engine not running yet - the native "
                                             "result now, the deep pass when it is (" + juce::String(file.path) + ")");
                    result.inputs_failed |= analysis_inputs::serviceInputs(!localRefPath.empty());
                }
                else if (target.run_deep_analysis) {
                    const auto t0 = juce::Time::getMillisecondCounter();
                    const auto wait = service_ready::waitForService(
                        [&service] { return service->available(); }, superseded,
                        [t0] { return (int)(juce::Time::getMillisecondCounter() - t0); },
                        [](int ms) { juce::Thread::sleep(ms); });
                    if (wait.cancelled) return;
                    engineReady = wait.available;
                    if (!wait.available) {
                        juce::Logger::writeToLog("AnalysisController: analysis engine did not answer /health in "
                                                 + juce::String(wait.waitedMs) + " ms (" + juce::String(wait.probes)
                                                 + " probes) - fast analysis only for " + juce::String(file.path));
                        // Asked for and not obtained (FIX-2): everything this
                        // run would have requested. A session with deep
                        // analysis switched off never gets here, so its
                        // result records nothing as failed.
                        result.inputs_failed |= analysis_inputs::serviceInputs(!localRefPath.empty());
                    }
                    else if (wait.available && wait.probes > 1)
                        juce::Logger::writeToLog("AnalysisController: analysis engine answered after "
                                                 + juce::String(wait.probes) + " probes / "
                                                 + juce::String(wait.waitedMs) + " ms");
                }
                // --- sections, drums, reference, chords, trust map ---
                // Independent requests, so they run at the same time
                // (PERF-2): one waits on the slowest instead of the sum.
                // Each has its own bridge; every future is joined below,
                // before the result is assembled. Once sent they are not
                // aborted: a run superseded from then on still waits for
                // them, bounded by each request's own timeout. An empty
                // call is a scan this pipeline does not need (the fix
                // check reads no chords or reference deltas).
                struct Scans {
                    std::future<Result<SectionAnalysis, ipc::BridgeError>> sections;
                    std::future<Result<AnalysisResult::DrumData, ipc::BridgeError>> drums;
                    std::optional<std::future<Result<ReferenceDeltas, ipc::BridgeError>>> reference;
                    std::optional<std::future<Result<ChordAnalysis, ipc::BridgeError>>> chords;
                    std::future<Result<juce::var, ipc::BridgeError>> trust;
                };
                std::optional<Scans> scans;
                const std::string path = file.path;
                const auto startScans = [&scans, &service, &path, &localRefPath] {
                    Scans s;
                    s.sections = std::async(std::launch::async, [service, path] { return service->sections(path); });
                    s.drums = std::async(std::launch::async, [service, path] { return service->drums(path); });
                    if (!localRefPath.empty() && service->reference)
                        s.reference = std::async(std::launch::async, [service, path, localRefPath] {
                            return service->reference(path, localRefPath);
                        });
                    if (service->chords)
                        s.chords = std::async(std::launch::async, [service, path] { return service->chords(path); });
                    // Trust map (pipeline #5): best-effort - on failure the map
                    // stays empty and the engine's trust pass is a no-op. The
                    // second generate() below down-weights recommendations
                    // built on tentative or unreliable measurements.
                    s.trust = std::async(std::launch::async, [service, path] { return service->trust(path); });
                    scans.emplace(std::move(s));
                };

                // OVH-1: a deep pass that does not run (switched off, or no
                // engine) is reported as skipped, so the progress strips show
                // its stages as skipped rather than done.
                if (!engineReady && singleFile) reportProgress("deep_analysis:skipped", 2, 6);
                if (engineReady) {
                    if (singleFile) reportProgress("deep_analysis", 2, 6);
                    // The fix check sends them with the deep job rather than
                    // after it: its wait is the slower of the two, not the sum.
                    if (service->scansWithDeep) startScans();

                    // Reuse the deep response Session Intake already computed for
                    // this exact file (path+mtime matched on the UI thread) instead
                    // of paying the 15-60 s Python pass a second time.
                    auto preIt = preloadedDeep.find(file.path);
                    if (preIt != preloadedDeep.end()) {
                        applyDeepResponse(result, preIt->second.response);
                    } else {
                        // Job-poll deep analysis with a wall-clock deadline,
                        // consecutive-error budget, and cooperative cancellation -
                        // now shared by BOTH the single-file and batch (>1 file)
                        // paths (audit CQ-04). The batch path previously used a
                        // single blocking analyze() call with none of these, so a
                        // stalled service mid-batch or a session switch couldn't be
                        // bounded/cancelled. Only the single-file path reports live
                        // sub-progress (batch progress reporting stays out of scope
                        // per the Global Constraints); both now track the current
                        // job id so cancel() can POST /cancel for the in-flight item.
                        DeepAnalysisJobHooks hooks;
                        hooks.shouldCancel = superseded;
                        hooks.onJobId = [state, myGen](const std::string& jobId) {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            // Only this generation's own thread reaches here, and only
                            // while it's still current, so this can't clobber a newer
                            // request's tracking.
                            if (state->owner != nullptr && state->generation.load() == myGen) {
                                state->currentJobId = jobId;
                                state->currentJobGeneration = myGen;
                            }
                        };
                        if (singleFile)
                            hooks.onProgress = [reportProgress](int stageIndex, int totalStages) {
                                // The sub-phase number is smuggled through the stage
                                // string ("deep_analysis:<stage_index>:<total_stages>")
                                // since ProgressCallback has no room for a second pair
                                // of ints; DashboardScreen parses it back out.
                                reportProgress("deep_analysis:" + std::to_string(stageIndex) + ":" +
                                                   std::to_string(totalStages),
                                               2, 6);
                            };

                        auto deepResult = service->deep(file.path, hooks);

                        {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            if (state->currentJobGeneration == myGen) {
                                state->currentJobId.clear();
                                state->currentJobGeneration = -1;
                            }
                        }
                        // Cancelled while the deep job ran: the five requests
                        // below would each cost the engine a scan whose answer
                        // is thrown away, and hold this pool's one thread - and
                        // the run that replaced this one - until the slowest
                        // came back. (The fix check sent them already; leaving
                        // joins them, bounded by their timeouts.)
                        if (deepResult.outcome == DeepAnalysisOutcome::Cancelled || superseded()) return;
                        if (deepResult.outcome == DeepAnalysisOutcome::FailedServerSide && singleFile)
                            reportProgress("deep_analysis:failed", 2, 6);
                        if (deepResult.hasResponse) applyDeepResponse(result, deepResult.response);
                        else if (deepResult.engineDidNotAnswer) result.inputs_failed |= analysis_inputs::kDeepResponse;
                    }

                    if (superseded()) return;
                    if (singleFile) reportProgress("sections", 3, 6);
                    if (!scans) startScans();

                    // A request the engine did not answer (down, timed out,
                    // shedding load) is recorded, so recovery knows a re-run
                    // can add it. An error about the request itself (404,
                    // 422, ...) is an answer: asking again changes nothing.
                    const auto noteUnanswered = [&result](const auto& response, analysis_inputs::Mask input) {
                        if (response.is_err() && ipc::engineDidNotAnswer(response.error()))
                            result.inputs_failed |= input;
                    };

                    auto sections = scans->sections.get();
                    if (sections.is_ok()) result.section_analysis = sections.value();
                    noteUnanswered(sections, analysis_inputs::kSections);
                    if (singleFile) reportProgress("drums", 4, 6);
                    auto drumsResult = scans->drums.get();
                    if (drumsResult.is_ok()) result.drums = drumsResult.value();
                    noteUnanswered(drumsResult, analysis_inputs::kDrums);
                    if (scans->reference.has_value()) {
                        if (singleFile) reportProgress("reference", 5, 6);
                        auto refResult = scans->reference->get();
                        if (refResult.is_ok()) result.reference_deltas = refResult.value();
                        noteUnanswered(refResult, analysis_inputs::kReference);
                    }
                    if (scans->chords.has_value()) {
                        if (singleFile) reportProgress("chords", 5, 6);
                        auto chordsResult = scans->chords->get();
                        if (chordsResult.is_ok()) result.chord_analysis = chordsResult.value();
                        noteUnanswered(chordsResult, analysis_inputs::kChords);
                    }

                    auto trustResult = scans->trust.get();
                    noteUnanswered(trustResult, analysis_inputs::kTrust);
                    if (trustResult.is_ok()) {
                        if (auto* ms = trustResult.value()["measurements"].getDynamicObject()) {
                            for (const auto& prop : ms->getProperties()) {
                                AnalysisResult::MeasurementTrust t;
                                t.confidence = static_cast<float>(static_cast<double>(prop.value["confidence"]));
                                t.quality_flag = prop.value["quality_flag"].toString().toStdString();
                                t.note = prop.value["note"].toString().toStdString();
                                result.measurement_trust[prop.name.toString().toStdString()] = t;
                            }
                        }
                    }
                }
                result.is_partial = false;
                if (result.inputs_failed != 0)
                    juce::Logger::writeToLog("AnalysisController: the engine did not answer for "
                                             + juce::String(analysis_inputs::describe(result.inputs_failed)) + " ("
                                             + juce::String(file.path) + ")");

                // Blend-aware genre resolution: result.genre_blend only exists
                // once deep analysis has run (this is the *second* generate()
                // call in this function - the first, above, is the fast-partial
                // preview and runs before genre_blend is available). Only
                // blends when the user left the genre unset in Intake, or their
                // explicit choice matches the auto-detected top genre - an
                // explicit choice that *disagrees* with detection is a
                // deliberate creative override and must not be second-guessed
                // by auto-detection.
                GenreEntry genreForRecs = genre;
                // BUG-1: the rule lives in GenreInUse.h, which the strip and
                // the sidebar read too; a detected (not stated) genre is
                // worded as such in the findings ("floor for detected X").
                if (const auto blendEntries = genre_in_use::blendEntries(genreId, result.genre_blend, genres);
                    !blendEntries.empty()) {
                    genreForRecs = blendGenreEntries(blendEntries);
                    if (genreId.empty())
                        genreForRecs.name = genre_in_use::detectedName(
                            genreForRecs.name.empty() ? genreForRecs.id : genreForRecs.name);
                }

                // Adapt to learned preferences on THIS file's copy. Adapting the
                // shared `genre` (as before) compounded the offsets once per
                // file in a batch, and a blend then discarded them entirely.
                if (localFeedbackMgr != nullptr) {
                    UserPreferenceModel prefs;
                    prefs.loadFromDatabase(*localFeedbackMgr);
                    prefs.adapt(genreForRecs);
                }

                result.recommendations = engine.generate(result, genreForRecs, target);

                batchResults.push_back(result);
            }

            if (batchResults.empty()) {
                dispatchFailure("Could not analyze any file in this session - the audio "
                                "may be missing, corrupt, or in an unsupported format.");
                return;
            }

            // Formulate final result (audit CQ-07: extracted into a pure,
            // independently-tested aggregator - see BatchResultAggregator.cpp
            // for the field-by-field mean/max/representative-file policy and
            // per-recommendation source_file_id tagging).
            AnalysisResult finalResult = aggregateBatchResults(batchResults, sessionId);

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->owner == nullptr || state->generation.load() != myGen) return;
            }

            juce::MessageManager::callAsync([state, myGen, finalResult, onUpdate, sessionId, currentMtime] {
                if (!state->mayDeliver(myGen)) return;
                auto* self = state->owner; // message thread: alive, as mayDeliver() just said
                self->current_ = finalResult;
                self->hasResult_ = true;
                self->cachedSessionId_ = sessionId;
                self->cachedFileMtime_ = currentMtime;
                // The listener gets its own copy: it may clear() or start a
                // new run, which resets current_ under it.
                if (onUpdate) onUpdate(finalResult);
            });
        } catch (const std::exception& e) {
            dispatchFailure("Analysis failed: " + juce::String(e.what()));
        } catch (...) {
            dispatchFailure("Analysis failed with an unknown error.");
        }
    });
}

} // namespace khensu
