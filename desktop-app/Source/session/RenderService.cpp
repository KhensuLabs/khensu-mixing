// desktop-app/Source/session/RenderService.cpp
// Moved from AppController_Render.cpp (ENG-1 stage C2); behaviour unchanged.
#include "RenderService.h"
#include "PlaybackService.h"
#include "SessionService.h"
#include "RecommendationChain.h"
#include <khensu/recommendations/MaskingChain.h>
#include "ReportBody.h"
#include "../BackgroundReadKey.h"
#include "../IBackgroundWork.h"
#include "../export/AudioExporter.h"
#include "../services/IAnalysisHub.h"
#include "../services/IStems.h"
#include "../services/EqBandChain.h"
#include "../ui/shared/KhensuTheme.h" // the report's palette snapshot
#include "../ui/shared/RenderPaths.h"
#include "../ui/shared/ServiceErrorHint.h"
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/ui/EqCurveComponent.h> // EqBand
#include <cmath>

// Service JSON field names, generated from resources/api/openapi.json:
// a renamed Python field is a compile error here, not a silent 0.
namespace apif = khensu::api::fields;

namespace khensu {

RenderService::RenderService(IBackgroundWork& work, SessionService& sessions, PlaybackService& playback, IStems& stems,
                             IAnalysisHub& analysis)
    : work_(work), sessions_(sessions), playback_(playback), stems_(stems), analysis_(analysis)
{
    // Prime the A/B loudness cache from the render's own guardrail: it
    // measured the render against its input at matched level, so a render
    // of the original is matched without re-decoding it (AUDIT-038).
    renderJobs_.onRenderCompleted = [this](const juce::String& inputPath, const juce::String& outputPath,
                                           const juce::var& fullResult) {
        if (!analysis_.hasAnalysis() || outputPath.isEmpty() ||
            juce::File(inputPath) != juce::File(sessions_.activeSourcePath()))
            return;
        const juce::var matched = fullResult[apif::RenderJobResult::guardrail][apif::RenderGuardrail::matched_gain_db];
        if (matched.isVoid() || matched.isUndefined()) return;
        const float renderLufs =
            analysis_.currentAnalysis().loudness.lufs_integrated - static_cast<float>((double)matched);
        if (std::isfinite(renderLufs)) playback_.rememberLufs(outputPath, renderLufs);
    };
    renderJobs_.setStemProvenanceProvider([this] {
        std::map<juce::String, juce::String> paths;
        for (const char* name : {"vocals", "drums", "bass", "other"}) {
            const auto stem = stems_.getStemFile(name);
            if (stem.existsAsFile()) paths.emplace(name, stem.getFullPathName());
        }
        return paths;
    });
}

RenderService::~RenderService() = default;

void RenderService::dispatchReportRequest(const juce::String& endpoint, const juce::String& jsonBody, Done callback)
{
    juce::WeakReference<RenderService> self(this);
    work_.runLongInBackground([endpoint, jsonBody, cb = std::move(callback), self]() mutable { // PDF/report render
        // Through the shared bridge path (was hand-rolled: no read deadline,
        // no token re-read on 401, a duplicated token-file lookup). Report /
        // PDF rendering can take a while, so allow a generous timeout.
        // (kDefaultBaseUrl is PythonServiceManager::kBaseUrl.)
        ipc::HttpServiceBridge bridge(ipc::HttpServiceBridge::kDefaultBaseUrl, 120000, 0);
        const auto result = bridge.postAnalyzeJson(endpoint.toStdString(), juce::JSON::parse(jsonBody));
        juce::String message;
        bool ok = false;
        if (result.is_err()) {
            message = serviceErrorLine(result.error()); // per-status hint (AUDIT-068)
        } else {
            message = result.value()[apif::ReportResponse::saved_path].toString();
            ok = message.isNotEmpty();
            if (!ok) message = "Missing saved_path in response";
        }
        juce::MessageManager::callAsync([ok, message, cb = std::move(cb), self]() mutable {
            if (self == nullptr) return;
            cb(ok, message);
        });
    });
}

void RenderService::exportAudio(const AudioExportOptions& opts, Done callback)
{
    // The weak reference guards the callback against use-after-free if the
    // owner is destroyed while the export thread is still running (Bug #6 fix).
    juce::WeakReference<RenderService> self(this);
    work_.runLongInBackground([opts, cb = std::move(callback), self]() mutable { // export can take minutes
        auto err = AudioExporter::exportFile(opts);
        juce::MessageManager::callAsync([err, opts, cb = std::move(cb), self]() mutable {
            if (self == nullptr) return; // destroyed - drop callback safely
            cb(err.isEmpty(), err.isEmpty() ? opts.outputPath : err);
        });
    });
}

void RenderService::generateReport(const juce::String& outputPath, const juce::String& reportMode, Done callback)
{
    if (!analysis_.hasAnalysis() || !sessions_.hasSession()) {
        callback(false, TRANS("No analysis available"));
        return;
    }
    dispatchReportRequest("/report",
                          report::buildRequestBody(analysis_.currentAnalysis(), sessions_.active(), outputPath, reportMode,
                                                   analysis_.scanResults()),
                          std::move(callback));
}

void RenderService::generatePdf(const juce::String& outputPath, const juce::String& reportMode, Done callback)
{
    if (!analysis_.hasAnalysis() || !sessions_.hasSession()) {
        callback(false, TRANS("No analysis available"));
        return;
    }
    dispatchReportRequest("/report/pdf",
                          report::buildRequestBody(analysis_.currentAnalysis(), sessions_.active(), outputPath, reportMode,
                                                   analysis_.scanResults()),
                          std::move(callback));
}

void RenderService::renderDeltaSolo(const juce::String& wetPath, Done onDone)
{
    if (!sessions_.state().session || sessions_.state().session->files.empty()) {
        if (onDone) onDone(false, TRANS("No active session."));
        return;
    }
    const juce::String wet = wetPath.isNotEmpty() ? wetPath : playback_.abProcessedPath();
    if (wet.isEmpty() || !juce::File(wet).existsAsFile()) {
        if (onDone) onDone(false, TRANS("No processed render to compare against."));
        return;
    }

    const juce::File dryFile(sessions_.activeSourcePath());
    const juce::File out = uniqueRenderFile(dryFile, "_delta");
    const juce::String dryPath = dryFile.getFullPathName();
    const juce::String outPath = out.getFullPathName();
    // Boost the (usually very quiet) delta so small differences are
    // audible at normal listening volume.
    constexpr float kDeltaBoostDb = 12.0f;
    // Captured so a completed delta-solo render from a session the user has
    // since left doesn't silently switch playback out from under them
    // (audit CQ-06 — the same pattern used for waveform/vocal-fallback/
    // stem-solo completion).
    const juce::String sessionId(sessions_.state().session->id);
    const auto generation = deltaSoloGeneration_.fetch_add(1) + 1;
    deltaSoloWetPath_ = wet.toStdString();
    const BackgroundReadKey readKey{sessionId.toStdString(), dryPath.toStdString(), wet.toStdString(), generation};

    juce::WeakReference<RenderService> self(this);
    work_.runLongInBackground([self, dryPath, wet, outPath, readKey, onDone]() mutable {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.renderDeltaSolo(dryPath, wet, outPath, kDeltaBoostDb);
        juce::MessageManager::callAsync([self, result = std::move(result), outPath, readKey, onDone]() mutable {
            if (self == nullptr) {
                if (onDone) onDone(false, TRANS("Window closed before the render finished."));
                return;
            }
            const auto& state = self->sessions_.state();
            const std::string currentSessionId = state.session ? state.session->id : std::string{};
            const std::string currentDryPath =
                state.session && !state.session->files.empty()
                    ? juce::File(self->sessions_.activeSourcePath()).getFullPathName().toStdString()
                    : std::string{};
            const BackgroundReadKey currentKey{currentSessionId, currentDryPath, self->deltaSoloWetPath_,
                                               self->deltaSoloGeneration_.load()};
            if (!readKey.matches(currentKey)) {
                if (onDone) onDone(false, TRANS("The delta render was superseded by a newer selection."));
                return;
            }
            if (result.is_err()) {
                if (onDone) onDone(false, serviceErrorLine(result.error()));
                return;
            }
            self->playback_.loadAudioFileForPlayback(juce::File(outPath));
            self->playback_.playAudio();
            if (onDone) onDone(true, outPath);
        });
    });
}

bool RenderService::isPathInsideDirectory(const juce::File& file, const juce::File& dir)
{
    if (dir == juce::File() || file == juce::File()) return false;
    return file.isAChildOf(dir);
}

void RenderService::registerAppliedRender(const juce::String& renderedPath)
{
    if (renderedPath.isEmpty() || !juce::File(renderedPath).existsAsFile()) return;
    // UIX-01 guard: a stem or single-take render (pitch, word gain, ride,
    // comp, alignment edits write next to the stems) is NOT a version of the
    // mix. Registering it made a lone vocal the accepted render that Export,
    // the Report and the A/B then used. Those panels audition their result
    // instead; this refuses the path whoever calls it.
    if (isPathInsideDirectory(juce::File(renderedPath), stems_.getStemsDirectory())) {
        juce::Logger::writeToLog("AppController: refused to register a stem render as the mix render: " + renderedPath);
        return;
    }

    // A path already in the tree (snapshot restore, autosave restore) is made
    // current again rather than duplicated. Otherwise truncate any redo tail
    // and append the render as the new tip, as a child of the original: this
    // entry point restores a known render whose input is not recorded. A
    // render made in the app goes through registerRenderOf, parented at the
    // version it processed (WF-1). All in RenderHistory (audit M4); load
    // stays here (playback IO).
    if (const int existing = renderHistory_.findNode(renderedPath); existing >= 0)
        renderHistory_.restoreTreeNode(existing);
    else
        renderHistory_.registerRender(renderedPath);
    loadRenderAtCurrentIndex();
    acceptedRenderChanged();
    notify(renderTreeListeners_);
}

void RenderService::registerAppliedRender(const juce::String& renderedPath, const juce::String& expectedSessionId)
{
    // AUDIT-002: a render started for song A must never land in song B's
    // history / undo / A-B side / autosave / export. Reject - and delete
    // nothing: the file is the user's render of A and A may be reopened.
    const juce::String currentId = sessions_.activeSessionId();
    if (expectedSessionId.isEmpty() || currentId != expectedSessionId) {
        juce::Logger::writeToLog("AppController: dropped render for superseded session " + expectedSessionId + ": " +
                                 renderedPath);
        return;
    }
    registerAppliedRender(renderedPath);
}

void RenderService::registerDerivedRender(const juce::String& renderedPath, const juce::String& expectedSessionId)
{
    // Same guards as registerAppliedRender (AUDIT-002 session check, UIX-01
    // stem refusal); the node is parented at the current one because the
    // render was made from the current accepted render (CE-AG-1).
    const juce::String currentId = sessions_.activeSessionId();
    if (expectedSessionId.isEmpty() || currentId != expectedSessionId) return;
    if (renderedPath.isEmpty() || !juce::File(renderedPath).existsAsFile()) return;
    if (isPathInsideDirectory(juce::File(renderedPath), stems_.getStemsDirectory())) return;
    if (const int existing = renderHistory_.findNode(renderedPath); existing >= 0)
        renderHistory_.restoreTreeNode(existing);
    else
        renderHistory_.registerDerivedRender(renderedPath);
    loadRenderAtCurrentIndex();
    acceptedRenderChanged();
    notify(renderTreeListeners_);
}

void RenderService::setRenderStart(RenderStart start)
{
    if (start == renderStart_) return;
    renderStart_ = start;
    notify(renderTreeListeners_);
}

juce::String RenderService::renderInputPath() const
{
    // WF-1: renders process the current version, so fixes stack - unless the
    // user chose to start from the original, or nothing has been accepted
    // yet. Same rule as Export: an accepted render that is gone from disk
    // falls back to the original.
    const juce::String original = sessions_.activeSourcePath();
    if (renderStart_ == RenderStart::Original) return original;
    return renderHistory_.exportSource(original);
}

void RenderService::registerRenderOf(const juce::String& renderedPath, const juce::String& inputPath,
                                     const juce::String& expectedSessionId)
{
    // Same guards as registerAppliedRender (AUDIT-002 session check, UIX-01
    // stem refusal, a path already in the tree made current again).
    const juce::String currentId = sessions_.activeSessionId();
    if (expectedSessionId.isEmpty() || currentId != expectedSessionId) {
        juce::Logger::writeToLog("AppController: dropped render for superseded session " + expectedSessionId + ": " +
                                 renderedPath);
        return;
    }
    if (renderedPath.isEmpty() || !juce::File(renderedPath).existsAsFile()) return;
    if (isPathInsideDirectory(juce::File(renderedPath), stems_.getStemsDirectory())) {
        juce::Logger::writeToLog("AppController: refused to register a stem render as the mix render: " + renderedPath);
        return;
    }
    int newNode = -1; // AUTO-3: set when the render is a new version
    if (const int existing = renderHistory_.findNode(renderedPath); existing >= 0) {
        renderHistory_.restoreTreeNode(existing);
    } else {
        // A render of the original is a root; a render of an accepted
        // version is that version's child (WF-1: fixes stack).
        const bool fromOriginal = juce::File(inputPath) == juce::File(sessions_.activeSourcePath());
        renderHistory_.registerRenderFrom(renderedPath, fromOriginal ? juce::String() : inputPath);
        newNode = renderHistory_.currentNode();
    }
    loadRenderAtCurrentIndex();
    acceptedRenderChanged();
    notify(renderTreeListeners_);
    if (newNode >= 0) maybeOfferReplay(newNode);
}

const IRenders::ChainRender* RenderService::chainFor(const juce::String& path) const
{
    const auto it = versionChains_.find(path);
    return it != versionChains_.end() ? &it->second : nullptr;
}

RenderService::ReplayOffer RenderService::replayOfferFor(int newNode) const
{
    ReplayOffer offer;
    const auto& tree = renderHistory_.tree();
    if (newNode < 0 || newNode >= (int)tree.size()) return offer;
    offer.sessionId = sessions_.activeSessionId();
    offer.basePath = tree[(size_t)newNode].path;
    // An edit of the original counts only when the user stepped back to it;
    // "Start from original" deliberately leaves the stacked fixes out (FIX-3).
    if (tree[(size_t)newNode].parent < 0 && renderStart_ == RenderStart::Original) return offer;
    // The same fix re-made on the earlier version (Gentler, a new amount)
    // replaces its old render: it is not re-run on top of itself.
    const auto* made = chainFor(offer.basePath);
    const juce::String madeFor = made != nullptr ? made->sourceRecommendationId : juce::String();
    for (const int node : RenderHistory::stackedAbove(tree, newNode)) {
        const auto* chain = chainFor(tree[(size_t)node].path);
        if (chain == nullptr || chain->chain.empty()) {
            ++offer.unknownSteps;
            continue;
        }
        if (madeFor.isNotEmpty() && chain->sourceRecommendationId == madeFor) continue;
        offer.steps.push_back(*chain);
    }
    return offer;
}

void RenderService::maybeOfferReplay(int newNode)
{
    if (replay_.active || !onReplayOffer) return;
    auto offer = replayOfferFor(newNode);
    if (!offer.steps.empty()) onReplayOffer(offer);
}

bool RenderService::startReplay(const ReplayOffer& offer)
{
    const auto fail = [this](const juce::String& why) {
        if (onReplayProgress) onReplayProgress(0, 0, why, true, false);
        return false;
    };
    if (replay_.active) return fail(TRANS("Stacked fixes are already being re-run."));
    if (offer.steps.empty()) return fail(TRANS("Nothing to re-run."));
    if (offer.sessionId.isEmpty() || offer.sessionId != sessions_.activeSessionId())
        return fail(TRANS("The session changed - the stacked fixes were not re-run."));
    if (currentRenderPath() != offer.basePath)
        return fail(TRANS("The current version changed - the stacked fixes were not re-run."));
    replay_.active = true;
    ++replay_.token;
    replay_.sessionId = offer.sessionId;
    replay_.input = offer.basePath;
    replay_.steps = offer.steps;
    replay_.next = 0;
    runNextReplayStep();
    return true;
}

void RenderService::runNextReplayStep()
{
    const int total = (int)replay_.steps.size();
    if (replay_.next >= replay_.steps.size()) {
        finishReplay(true, TRANS("Re-ran %n stacked fixes on the new version.").replace("%n", juce::String(total)));
        return;
    }
    const auto& step = replay_.steps[replay_.next];
    const int index = (int)replay_.next + 1;
    const juce::File out = uniqueRenderFile(juce::File(sessions_.activeSourcePath()), "_rerun");
    ChainRender labelled = step;
    labelled.description = TRANS("Re-run") + " " + juce::String(index) + "/" + juce::String(total) + ": " +
                           step.description + " -> " + out.getFileName();
    replay_.runningDescription = labelled.description;
    versionChains_[out.getFullPathName()] = step; // a later branch can re-run it again
    if (onReplayProgress)
        onReplayProgress(index - 1, total,
                         TRANS("Re-running stacked fix %i of %n: %d")
                             .replace("%i", juce::String(index))
                             .replace("%n", juce::String(total))
                             .replace("%d", step.description),
                         false, true);
    juce::WeakReference<RenderService> self(this);
    const int token = replay_.token;
    // The step processes the previous step's output, not renderInputPath():
    // the queue rebuilds one chain whatever "Start from" says.
    startChain(labelled, replay_.input, out.getFullPathName(), true,
               [self, token](RenderOutcome outcome, const juce::String& pathOrError, const juce::var&) {
                   auto* s = self.get();
                   if (s == nullptr || !s->replay_.active || s->replay_.token != token) return;
                   switch (outcome) {
                   case RenderOutcome::Rendered:
                       s->replay_.input = pathOrError;
                       ++s->replay_.next;
                       s->runNextReplayStep();
                       return;
                   case RenderOutcome::Cancelled:
                       s->finishReplay(false, TRANS("Re-running the stacked fixes was cancelled."));
                       return;
                   case RenderOutcome::SessionChanged:
                       s->finishReplay(false, TRANS("The session changed - the stacked fixes were not re-run."));
                       return;
                   case RenderOutcome::Failed:
                       s->finishReplay(false, TRANS("A stacked fix did not render:") + " " + pathOrError);
                       return;
                   }
               });
}

void RenderService::finishReplay(bool ok, const juce::String& status)
{
    const int done = (int)replay_.next;
    const int total = (int)replay_.steps.size();
    replay_.active = false;
    ++replay_.token;
    replay_.steps.clear();
    replay_.runningDescription.clear();
    if (onReplayProgress) onReplayProgress(done, total, status, true, ok);
}

void RenderService::cancelReplay()
{
    if (!replay_.active) return;
    const auto running = replay_.runningDescription;
    finishReplay(false, TRANS("Re-running the stacked fixes was cancelled."));
    for (const auto& job : renderJobs_.activeJobs())
        if (running.isNotEmpty() && job.description == running) renderJobs_.cancelJob(job.localId);
}

juce::String RenderService::exportSourcePath() const
{
    return renderHistory_.exportSource(sessions_.activeSourcePath());
}

void RenderService::restoreRenderTreeNode(int nodeIndex)
{
    // Make the node current and re-sync the linear tip (RenderHistory returns
    // "" for an out-of-range index, matching the old no-op guard).
    if (renderHistory_.restoreTreeNode(nodeIndex).isEmpty()) return;
    loadRenderAtCurrentIndex();
    acceptedRenderChanged();
    notify(renderTreeListeners_);
}

RenderJobManager::Callback RenderService::chainCompletion(const juce::String& sessionId, const juce::String& inputPath,
                                                         bool asVersion, RenderDone onDone)
{
    juce::WeakReference<RenderService> self(this);
    return [self, onDone, sessionId, inputPath, asVersion](bool ok, const juce::String& pathOrError,
                                                           const juce::var& fullResult) {
        const auto report = [&](RenderOutcome outcome, const juce::String& text) {
            if (onDone) onDone(outcome, text, fullResult);
        };
        if (self == nullptr) return report(RenderOutcome::Failed, "Window closed before the render finished.");
        if (!ok) return report(pathOrError.isEmpty() ? RenderOutcome::Cancelled : RenderOutcome::Failed, pathOrError);
        // AUDIT-002: a render of song A never lands in song B.
        if (sessionId.isEmpty() || self->sessions_.activeSessionId() != sessionId)
            return report(RenderOutcome::SessionChanged, pathOrError);
        if (asVersion) self->registerRenderOf(pathOrError, inputPath, sessionId);
        report(RenderOutcome::Rendered, pathOrError);
    };
}

bool RenderService::shouldRenderNatively(bool serviceUp, const std::vector<ipc::HttpServiceBridge::EffectSpec>& chain)
{
    return !serviceUp && dsp::OfflineChainRenderer::canRender(nativeChain(chain));
}

std::vector<dsp::ChainStageSpec>
RenderService::nativeChain(const std::vector<ipc::HttpServiceBridge::EffectSpec>& chain)
{
    std::vector<dsp::ChainStageSpec> out;
    out.reserve(chain.size());
    for (const auto& e : chain) {
        dsp::ChainStageSpec s;
        s.effect = e.effect.toStdString();
        for (const auto& [name, value] : e.parameters) s.parameters[name.toStdString()] = value;
        out.push_back(std::move(s));
    }
    return out;
}

juce::var RenderService::nativeRenderResult(const dsp::OfflineRenderResult& result, const juce::String& outputPath)
{
    const auto strings = [](const std::vector<std::string>& v) {
        juce::Array<juce::var> a;
        for (const auto& s : v) a.add(juce::String(juce::CharPointer_UTF8(s.c_str())));
        return juce::var(a);
    };
    const auto metrics = [](const dsp::RenderLevels& l) {
        juce::DynamicObject::Ptr m = new juce::DynamicObject();
        m->setProperty(apif::GuardrailMetrics::true_peak_dbtp, std::round(l.truePeakDbtp * 100.0) / 100.0);
        m->setProperty(apif::GuardrailMetrics::mono_low_corr, std::round(l.monoLowCorrelation * 1000.0) / 1000.0);
        return juce::var(m.get());
    };
    juce::DynamicObject::Ptr guard = new juce::DynamicObject();
    guard->setProperty(apif::RenderGuardrail::ok, result.guardrailOk);
    guard->setProperty(apif::RenderGuardrail::matched_gain_db, result.matchedGainDb);
    guard->setProperty(apif::RenderGuardrail::before, metrics(result.before));
    guard->setProperty(apif::RenderGuardrail::after, metrics(result.after));
    guard->setProperty(apif::RenderGuardrail::blocks, strings(result.blocks));
    guard->setProperty(apif::RenderGuardrail::warnings, strings(result.warnings));
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::RenderJobResult::status, "success");
    o->setProperty(apif::RenderJobResult::output_path, outputPath);
    o->setProperty(apif::RenderJobResult::applied_effects, strings(result.applied));
    o->setProperty(apif::RenderJobResult::skipped_effects, juce::Array<juce::var>());
    o->setProperty(apif::RenderJobResult::guardrail, juce::var(guard.get()));
    o->setProperty("renderer", "native"); // not a service field: which engine rendered it
    return juce::var(o.get());
}

void RenderService::startChain(const ChainRender& render, const juce::String& inputPath,
                               const juce::String& outputPath, bool asVersion, RenderDone onDone)
{
    // OVH-9: without the engine, render what the native renderer covers here
    // rather than queue a render nothing will answer.
    const bool serviceUp = !serviceAvailable || serviceAvailable();
    if (render.region) return startRegionChain(render, inputPath, outputPath, asVersion, std::move(onDone));
    if (shouldRenderNatively(serviceUp, render.chain))
        return startNativeChain(render, inputPath, outputPath, asVersion, std::move(onDone));
    const juce::String sessionId = sessions_.activeSessionId();
    renderJobs_.startChainRender(render.description, inputPath, outputPath, render.chain, sessionId,
                                 render.sourceRecommendationId,
                                 chainCompletion(sessionId, inputPath, asVersion, std::move(onDone)),
                                 render.genreRelative, render.genreActionAllowed, render.abstainedReason,
                                 render.targetStem, render.decisionHash, render.keyStem);
}

void RenderService::startRegionChain(const ChainRender& render, const juce::String& inputPath,
                                     const juce::String& outputPath, bool asVersion, RenderDone onDone)
{
    // VIZ-N3: one section with the chain, the rest of the track copied from
    // the input, crossfaded at both edges (the service's section render).
    const juce::String sessionId = sessions_.activeSessionId();
    renderJobs_.rememberChain(render.chain, render.decisionHash);
    ipc::HttpServiceBridge::SectionChainSpec spec;
    spec.label = render.regionLabel.isNotEmpty() ? render.regionLabel : juce::String("region");
    spec.startSec = render.region->first;
    spec.endSec = render.region->second;
    spec.effects = render.chain;
    renderJobs_.startLocalRender(
        render.description, inputPath, sessionId, render.sourceRecommendationId,
        [inputPath, outputPath, spec, sessionId, rec = render.sourceRecommendationId](const std::atomic<bool>& stop) {
            RenderJobManager::LocalRenderOutcome outcome;
            ipc::HttpServiceBridge bridge;
            const auto started = bridge.startSectionChainJob(inputPath, outputPath, {spec}, 80.0, sessionId, rec);
            if (started.is_err()) {
                outcome.outputPathOrError = juce::String(started.error().message);
                return outcome;
            }
            const auto jobId = started.value();
            for (int polls = 0; polls < 3600 * 4; ++polls) {
                if (stop.load()) {
                    bridge.cancelSectionChainJob(jobId);
                    return outcome; // a cancel: ok = false, no error
                }
                const auto st = bridge.getSectionChainJobStatus(jobId);
                if (st.is_ok()) {
                    const auto& v = st.value();
                    if (v.status == "completed") {
                        outcome.ok = true;
                        outcome.outputPathOrError = v.outputPath.isNotEmpty() ? v.outputPath : outputPath;
                        juce::DynamicObject::Ptr full = new juce::DynamicObject();
                        full->setProperty("output_path", outcome.outputPathOrError);
                        full->setProperty("status", "success");
                        full->setProperty("guardrail", v.guardrail);
                        outcome.fullResult = juce::var(full.get());
                        return outcome;
                    }
                    if (v.status == "failed" || v.status == "cancelled") {
                        outcome.outputPathOrError = v.error.isNotEmpty() ? v.error : juce::String("Section render failed");
                        return outcome;
                    }
                }
                juce::Thread::sleep(250);
            }
            outcome.outputPathOrError = "Section render timed out";
            return outcome;
        },
        chainCompletion(sessionId, inputPath, asVersion, std::move(onDone)));
}

void RenderService::startNativeChain(const ChainRender& render, const juce::String& inputPath,
                                     const juce::String& outputPath, bool asVersion, RenderDone onDone)
{
    const juce::String sessionId = sessions_.activeSessionId();
    renderJobs_.rememberChain(render.chain, render.decisionHash);
    juce::Logger::writeToLog("RenderService: the analysis engine is not up - rendering natively: " +
                             render.description);
    // ENH-6: a keyed DynamicEQ listens to the leading stem when it has been
    // separated; without it the stage keys on its own input.
    juce::String keyPath;
    if (render.keyStem.isNotEmpty()) {
        const auto keyFile = stems_.getStemFile(render.keyStem);
        if (keyFile.existsAsFile()) keyPath = keyFile.getFullPathName();
    }
    renderJobs_.startLocalRender(
        render.description, inputPath, sessionId, render.sourceRecommendationId,
        [inputPath, outputPath, keyPath, chain = nativeChain(render.chain)](const std::atomic<bool>& stop) {
            dsp::OfflineRenderOptions options;
            options.shouldCancel = [&stop] { return stop.load(); };
            options.keyPath = keyPath.toStdString();
            const auto result =
                dsp::OfflineChainRenderer::renderFile(juce::File(inputPath), juce::File(outputPath), chain, options);
            RenderJobManager::LocalRenderOutcome outcome;
            switch (result.status) {
            case dsp::OfflineRenderResult::Status::Rendered:
                outcome.ok = true;
                outcome.outputPathOrError = outputPath;
                outcome.fullResult = nativeRenderResult(result, outputPath);
                break;
            case dsp::OfflineRenderResult::Status::Cancelled:
                break; // ok = false with an empty error: a cancel
            case dsp::OfflineRenderResult::Status::NeedsEngine:
            case dsp::OfflineRenderResult::Status::Failed:
                outcome.outputPathOrError = juce::String(juce::CharPointer_UTF8(result.error.c_str()));
                if (outcome.outputPathOrError.isEmpty()) outcome.outputPathOrError = "Render failed";
                break;
            }
            return outcome;
        },
        chainCompletion(sessionId, inputPath, asVersion, std::move(onDone)));
}

juce::File RenderService::renderVersion(const ChainRender& render, const juce::String& outputSuffix,
                                        RenderDone onDone)
{
    if (!sessions_.state().session || sessions_.state().session->files.empty()) {
        if (onDone) onDone(RenderOutcome::Failed, TRANS("No active session."), {});
        return {};
    }
    if (render.chain.empty()) {
        if (onDone) onDone(RenderOutcome::Failed, TRANS("Nothing to render."), {});
        return {};
    }
    // WF-1: process the current version (fixes stack) or the original, as
    // chosen; the output is named after the original either way.
    const juce::String inPath = renderInputPath();
    const juce::File outputFile = uniqueRenderFile(juce::File(sessions_.activeSourcePath()), outputSuffix);
    ChainRender labelled = render;
    labelled.description = render.description + " -> " + outputFile.getFileName();
    versionChains_[outputFile.getFullPathName()] = render; // AUTO-3: what made this version
    startChain(labelled, inPath, outputFile.getFullPathName(), true, std::move(onDone));
    return outputFile;
}

void RenderService::renderPreview(const ChainRender& render, const juce::String& inputPath,
                                  const juce::String& outputPath, RenderDone onDone)
{
    if (!sessions_.state().session || render.chain.empty()) {
        if (onDone)
            onDone(RenderOutcome::Failed, !sessions_.state().session ? TRANS("No active session.") : TRANS("Nothing to render."), {});
        return;
    }
    startChain(render, inputPath, outputPath, false, std::move(onDone));
}

void RenderService::reportThroughDone(const Done& onDone, RenderOutcome outcome, const juce::String& pathOrError)
{
    if (!onDone) return;
    switch (outcome) {
    case RenderOutcome::Rendered: onDone(true, pathOrError); return;
    case RenderOutcome::Failed: onDone(false, pathOrError); return;
    case RenderOutcome::Cancelled: onDone(false, TRANS("Render cancelled.")); return;
    case RenderOutcome::SessionChanged: onDone(false, TRANS("Session changed - the render was not loaded.")); return;
    }
}

void RenderService::applyRecommendationChain(const Recommendation& rec, Done onDone)
{
    ChainRender render;
    render.chain = buildRecommendationChain(rec);
    if (const auto dyn = maskingDynamicEq(rec)) render.keyStem = juce::String(dyn->keyStem);
    if (sessions_.state().session && !sessions_.state().session->files.empty() && render.chain.empty()) {
        if (onDone)
            onDone(false,
                   "This recommendation has no concrete parameters to render automatically - dial it in manually.");
        return;
    }
    const juce::String shortId = juce::String(rec.id).substring(0, 8);
    render.description = "Fix " + shortId;
    render.sourceRecommendationId = juce::String(rec.id);
    renderVersion(render, "_fix_" + shortId, [onDone](RenderOutcome outcome, const juce::String& pathOrError,
                                                      const juce::var&) {
        reportThroughDone(onDone, outcome, pathOrError);
    });
}

void RenderService::applyEffectChain(const std::vector<ipc::HttpServiceBridge::EffectSpec>& chain,
                                     const juce::String& tag, Done onDone)
{
    const juce::String safeTag = tag.retainCharacters("abcdefghijklmnopqrstuvwxyz0123456789_").substring(0, 32);
    ChainRender render;
    render.description = tag;
    render.chain = chain;
    render.sourceRecommendationId = tag;
    renderVersion(render, "_" + (safeTag.isNotEmpty() ? safeTag : "chain"),
                  [onDone](RenderOutcome outcome, const juce::String& pathOrError, const juce::var&) {
                      reportThroughDone(onDone, outcome, pathOrError);
                  });
}

void RenderService::applyCleanupChain(const juce::String& effect, int presetIdx, Done onDone)
{
    ipc::HttpServiceBridge::EffectSpec spec;
    spec.effect = effect;
    spec.parameters["preset_idx"] = (float)presetIdx;
    ChainRender render;
    render.description = effect + " cleanup";
    render.chain = {spec};
    renderVersion(render, "_clean_" + effect.toLowerCase() + juce::String(presetIdx),
                  [onDone](RenderOutcome outcome, const juce::String& pathOrError, const juce::var&) {
                      reportThroughDone(onDone, outcome, pathOrError);
                  });
}

void RenderService::applyEqBands(const std::vector<EqBand>& bands, Done onDone)
{
    ChainRender render;
    render.chain = eqBandsToChain(bands); // the same chain the live preview plays (STU-9)
    if (sessions_.state().session && !sessions_.state().session->files.empty() && render.chain.empty()) {
        if (onDone) onDone(false, TRANS("No EQ bands with a non-zero gain to render - drag a band up or down first."));
        return;
    }
    render.description = "EQ curve";
    render.sourceRecommendationId = "eq_curve_apply";
    renderVersion(render, "_eq", [onDone](RenderOutcome outcome, const juce::String& pathOrError, const juce::var&) {
        reportThroughDone(onDone, outcome, pathOrError);
    });
}

void RenderService::saveNamedSnapshot(const juce::String& label, std::function<void(bool)> onSaved)
{
    if (!sessions_.state().session || sessions_.state().session->files.empty() || !analysis_.hasAnalysis()) {
        if (onSaved) onSaved(false);
        return;
    }

    const juce::String rendered = renderHistory_.currentPath();
    const juce::String currentPath = rendered.isNotEmpty() ? rendered : sessions_.activeSourcePath();

    const auto& r = analysis_.currentAnalysis();
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty("rendered_path", currentPath);
    payload->setProperty("lufs_integrated", r.loudness.lufs_integrated);
    payload->setProperty("true_peak_dbtp", r.loudness.true_peak_dbtp);
    payload->setProperty("dynamic_range_db", r.dynamics.dynamic_range_db);
    payload->setProperty("stereo_width", r.stereo.width);

    const juce::String sessionId = juce::String(sessions_.state().session->id);
    juce::WeakReference<RenderService> self(this);
    work_.runInBackground([self, sessionId, label, payload, onSaved]() {
        ipc::HttpServiceBridge bridge;
        const bool ok = bridge.postEvent(sessionId, "snapshot_created", label, {}, juce::var(payload.get()));
        if (onSaved)
            juce::MessageManager::callAsync([self, onSaved, ok]() {
                if (self != nullptr) onSaved(ok);
            });
    });
}

void RenderService::undoRender()
{
    if (!canUndoRender()) return;
    renderHistory_.undo();
    loadRenderAtCurrentIndex();
    acceptedRenderChanged();
    notify(renderTreeListeners_); // the tree panel highlights the current node
}

void RenderService::redoRender()
{
    if (!canRedoRender()) return;
    renderHistory_.redo();
    loadRenderAtCurrentIndex();
    acceptedRenderChanged();
    notify(renderTreeListeners_); // the tree panel highlights the current node
}

void RenderService::loadRenderAtCurrentIndex()
{
    // The B side follows the render history; PlaybackService keeps position
    // and play state and levels B (cached or measured LUFS).
    playback_.loadAcceptedRender(renderHistory_.currentPath());
}

juce::String RenderService::describeRenderAt(int indexOffset) const
{
    const int idx = renderHistory_.currentIndex() + indexOffset;
    if (idx < 0) return "original mix";
    const auto& applied = renderHistory_.applied();
    if (idx >= (int)applied.size()) return {};
    return juce::File(applied[(size_t)idx]).getFileName();
}

void RenderService::resetForSession()
{
    // Every queued/running render belongs to the session being left: cancel
    // them so none can finish into the next session's history, A/B side,
    // autosave or export (AUDIT-002). Callbacks fire now with ok=false.
    if (replay_.active) finishReplay(false, TRANS("The session changed - the stacked fixes were not re-run."));
    ++replay_.token;
    versionChains_.clear();
    renderJobs_.cancelAll();
    renderHistory_.clear();
    renderStart_ = RenderStart::CurrentVersion; // WF-1: each session starts stacking
    deltaSoloWetPath_.clear();
    ++deltaSoloGeneration_; // drop any in-flight delta render for the old session
}

} // namespace khensu
