// desktop-app/Source/session/StemService.cpp
// Moved from AppController_Render.cpp / AppController_Session.cpp /
// AppController.cpp (ENG-1 stage C3); behaviour unchanged.
#include "StemService.h"
#include "KeyedListeners.h"
#include "SessionService.h"
#include "../BackgroundReadKey.h"
#include "../IBackgroundWork.h"
#include "../PythonServiceManager.h"
#include "../analysis/ServiceReadyWait.h"
#include "../ui/shared/JobPoller.h"
#include <khensu/analysis/AudioAnalyzer.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiJobStatus.h>

namespace khensu {

StemService::StemService(IBackgroundWork& work, SessionService& sessions) : work_(work), sessions_(sessions) {}

StemService::~StemService() = default;

void StemService::notifyVocalAnalysisListeners()
{
    notifyKeyedListeners(vocalAnalysisListeners_);
}

// ---- Stems on disk -------------------------------------------------------------

juce::File StemService::getStemsDirectory() const
{
    if (!sessions_.state().session || sessions_.state().session->files.empty()) return {};
    juce::File audioFile(sessions_.activeSourcePath());
    const auto parent = audioFile.getParentDirectory();

    // Cached: every getStemFile() call (dozens per screen refresh) used to
    // list the source's whole parent folder. Reuse the last answer for the
    // same source while that folder exists; re-resolve a not-yet-created
    // folder at most every 2 s so a fresh separation is picked up.
    const auto nowMs = juce::Time::getMillisecondCounter();
    {
        const juce::ScopedLock sl(stemsDirCacheLock_);
        if (stemsDirCacheSource_ == audioFile && stemsDirCacheSource_ != juce::File() &&
            (stemsDirCache_.isDirectory() || nowMs - stemsDirCacheMs_ < 2000))
            return stemsDirCache_;
    }
    const auto remember = [&](const juce::File& dir) {
        const juce::ScopedLock sl(stemsDirCacheLock_);
        stemsDirCacheSource_ = audioFile;
        stemsDirCache_ = dir;
        stemsDirCacheMs_ = nowMs;
        return dir;
    };

    // AMAS_<M>_<D>_<YYYY>_<SongTitle> (spec §1.3) - mirrors
    // khensu_services/util/naming.py; change both together.
    const juce::String stem = audioFile.getFileNameWithoutExtension();
    juce::String safe = stem.retainCharacters("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                              "abcdefghijklmnopqrstuvwxyz0123456789_-");
    if (safe.isEmpty()) safe = "Untitled";
    // AUDIT-074: a sanitisation that dropped more than spaces (accents,
    // non-Latin names) gets "_" + 8 lowercase hex digits of FNV-1a-32 over the
    // UTF-8 stem appended, so two such songs in one folder never share a
    // stems folder.
    if (safe != stem.removeCharacters(" ")) {
        juce::uint32 h = 2166136261u;
        for (auto* p = stem.toRawUTF8(); *p != 0; ++p) {
            h ^= static_cast<juce::uint8>(*p);
            h *= 16777619u;
        }
        safe << "_" << juce::String::toHexString(static_cast<juce::int64>(h)).paddedLeft('0', 8);
    }

    // Reuse the newest existing AMAS folder for this title (any date):
    // stems act as a cache, not one folder per day.
    juce::File newest;
    for (const auto& d : parent.findChildFiles(juce::File::findDirectories, false, "AMAS_*_" + safe)) {
        if (d.getFileName().endsWith("_" + safe) &&
            (!newest.exists() || d.getLastModificationTime() > newest.getLastModificationTime()))
            newest = d;
    }
    if (newest.exists()) return remember(newest);

    const auto t = juce::Time::getCurrentTime();
    return remember(parent.getChildFile("AMAS_" + juce::String(t.getMonth() + 1) + "_" +
                                        juce::String(t.getDayOfMonth()) + "_" + juce::String(t.getYear()) + "_" +
                                        safe));
}

juce::File StemService::getStemFile(const juce::String& stemName) const
{
    return getStemsDirectory().getChildFile(stemName + ".wav");
}

bool StemService::stemFilesExist() const
{
    if (!sessions_.state().session) return false;
    for (const char* name : {"vocals", "drums", "bass", "other"})
        if (!getStemFile(name).existsAsFile()) return false;
    return true;
}

StemSeparationInfo StemService::stemSeparationInfo() const
{
    auto info = StemSeparationInfo::readFrom(getStemsDirectory());
    // No separation.json (stems from an older build, or the service did not
    // write one): fall back to what this run's separation reported.
    if (!info.known() && sessions_.state().lastSeparationResult.has_value())
        info.method = juce::String(sessions_.state().lastSeparationResult->method_used);
    return info;
}

bool StemService::hasStemsAvailable() const
{
    // AID-07: the HPSS fallback writes the same four file names, so existence
    // alone made every stem consumer treat a harmonic/percussive guess as
    // real separated stems.
    return stemFilesExist() && !stemSeparationInfo().isApproximate();
}

bool StemService::isSeparatingStems() const noexcept { return sessions_.state().separatingStems; }

const std::optional<ipc::StemSeparationResult>& StemService::getLastSeparationResult() const noexcept
{
    return sessions_.state().lastSeparationResult;
}

// ---- Separation ----------------------------------------------------------------

void StemService::runStemSeparation(const juce::String& method,
                                    std::function<void(bool success, juce::String message)> callback)
{
    if (!sessions_.hasSession()) {
        callback(false, "No active session loaded");
        return;
    }

    const auto* s = sessions_.active();
    if (s->files.empty()) {
        callback(false, "No audio files in active session");
        return;
    }

    const std::string file_path = s->files[0].path;
    const std::string output_dir = getStemsDirectory().getFullPathName().toStdString();
    const std::string method_str = method.toStdString();
    // Captured so a separation started for a session the user has since left
    // doesn't overwrite the current session's lastSeparationResult/
    // vocalAnalysis with stale data (audit CQ-06 — a full-length Demucs run
    // can take minutes, plenty of time to switch sessions).
    const std::string sessionId = s->id;

    sessions_.state().separatingStems = true;
    // AUDIT-067: resetForSession() bumps this, which stops the poll loop,
    // cancels the server job and keeps the stale completion away from the
    // next session's separatingStems flag.
    const auto separationGeneration = separationGeneration_.load();

    juce::WeakReference<StemService> self(this);
    // Separation polls for up to 30 min: long pool, never the short one.
    // `this` is safe on the worker: the long pool is drained in the
    // owner's destructor before this service dies.
    work_.runLongInBackground([this, identityWaitMs = identityWaitMs_.load(), file_path, output_dir, method_str, sessionId, separationGeneration,
                               cb = std::move(callback), self]() mutable {
        // Demucs (the "advanced"/AI method) can take 1-3 minutes on CPU for a
        // full track - well past the default HTTP timeout, so the poll loop
        // below (not a single blocking call) is what actually waits it out.
        ipc::HttpServiceBridge bridge(PythonServiceManager::kBaseUrl, 300000, 2000);
        // A short-timeout bridge for the polls themselves: a status read that
        // takes minutes means the service is gone, not busy.
        ipc::HttpServiceBridge pollBridge(PythonServiceManager::kBaseUrl, 15000, 2000);

        khensu::Result<ipc::SeparationJobStatus, ipc::BridgeError> finalStatus =
            khensu::Result<ipc::SeparationJobStatus, ipc::BridgeError>::err(
                {ipc::BridgeError::Kind::ServiceUnavailable, "Stem separation could not start", 0});

        // DSK-10 / S5: the shared JobPoller - a bounded consecutive-error
        // budget (a crashed service ends the wait with "service lost" within
        // ~10 s instead of holding this worker for 30 minutes), 404 =
        // terminal, a wall-clock deadline, and a server-side cancel when the
        // session changes or the wait is abandoned.
        // The bridge refuses every request while the engine's identity is
        // unproven - before the first HMAC /health answer, or after a busy
        // service (a cold deep pass) missed its probes - and a one-shot start
        // then failed the whole separation with "service identity not
        // verified" while the engine was healthy. Give the gate the time the
        // health monitor needs to re-prove it (a probe every ~1.6 s, 2.5 s
        // timeout); a separation runs for minutes, so the wait is free.
        const auto sessionChanged = [this, separationGeneration] {
            return separationGeneration_.load() != separationGeneration;
        };
        service_ready::waitForService(
            [] {
                return !ipc::HttpServiceBridge::serviceIdentityRequired() ||
                       ipc::HttpServiceBridge::serviceIdentityVerified();
            },
                                      sessionChanged,
                                      [] { return static_cast<int>(juce::Time::getMillisecondCounter()); },
                                      [](int ms) { juce::Thread::sleep(ms); }, identityWaitMs);

        JobPoller<ipc::SeparationJobStatus>::Hooks hooks;
        hooks.start = [&] { return bridge.startSeparationJob(file_path, output_dir, method_str); };
        hooks.poll = [&](const std::string& id) { return pollBridge.getSeparationProgress(id); };
        hooks.cancel = [&](const std::string& id) { pollBridge.cancelSeparationJob(id); };
        hooks.isTerminal = [](const ipc::SeparationJobStatus& st) { return khensu::api::isTerminalStatus(st.status); };
        hooks.shouldStop = [this, separationGeneration] {
            return separationGeneration_.load() != separationGeneration; // session changed
        };
        hooks.onProgress = [self, separationGeneration](const ipc::SeparationJobStatus& status) {
            juce::MessageManager::callAsync([self, status, separationGeneration]() {
                if (self == nullptr || self->separationGeneration_.load() != separationGeneration) return;
                // Stage 6 of 6 - the fixed 6-stage numbering established in
                // AnalysisController (Task 5); status.progress carries the
                // real Demucs chunk-level fraction for the UI's inner fill
                // bar, smuggled through the stage string as
                // "stems:<status>:<fraction>" since ProgressCallback's
                // signature has no room for a float.
                const std::string stemStage = "stems:" + status.status + ":" + std::to_string(status.progress);
                if (self->onStemProgress) self->onStemProgress(stemStage, 6, 6);
            });
        };
        JobPoller<ipc::SeparationJobStatus>::Options pollOptions;
        pollOptions.pollIntervalMs = 500;
        pollOptions.maxConsecutiveErrors = 20; // ~10 s of failed polls at 500 ms
        pollOptions.deadlineSeconds = 30.0 * 60.0;
        const auto polled = JobPoller<ipc::SeparationJobStatus>::run(hooks, pollOptions);
        if (polled.finished()) {
            finalStatus = khensu::Result<ipc::SeparationJobStatus, ipc::BridgeError>::ok(*polled.status);
        } else if (polled.outcome != JobPollOutcome::StartFailed) {
            finalStatus = khensu::Result<ipc::SeparationJobStatus, ipc::BridgeError>::err(
                {polled.outcome == JobPollOutcome::ServiceLost ? ipc::BridgeError::Kind::ServiceUnavailable
                                                               : ipc::BridgeError::Kind::Timeout,
                 polled.describe().toStdString(), polled.lastError.http_status});
            // Tell the HUD the stems stage is over (it would otherwise keep
            // showing "STEMS" for a job nobody is waiting on).
            const std::string stage =
                polled.outcome == JobPollOutcome::Stopped ? "stems:cancelled:0" : "stems:failed:0";
            juce::MessageManager::callAsync([self, stage, separationGeneration]() {
                if (self == nullptr || self->separationGeneration_.load() != separationGeneration) return;
                if (self->onStemProgress) self->onStemProgress(stage, 6, 6);
            });
        } else {
            // StartFailed: say so, with the bridge's reason ("service identity
            // not verified", a 422 for the file, ...).
            finalStatus = khensu::Result<ipc::SeparationJobStatus, ipc::BridgeError>::err(
                {polled.lastError.kind, polled.describe().toStdString(), polled.lastError.http_status});
        }

        // AID-07: what actually produced the stems (separation.json written by
        // the service next to them), read here on the worker.
        const auto separationInfo = StemSeparationInfo::readFrom(juce::File(juce::String(output_dir)));
        std::optional<AnalysisResult> vocalRes;
        khensu::Result<ipc::StemSeparationResult, ipc::BridgeError> outcome =
            khensu::Result<ipc::StemSeparationResult, ipc::BridgeError>::err(
                {ipc::BridgeError::Kind::ServiceUnavailable, "Stem separation did not complete"});
        if (finalStatus.is_ok() && finalStatus.value().status == khensu::api::job_status::completed) {
            outcome = khensu::Result<ipc::StemSeparationResult, ipc::BridgeError>::ok(finalStatus.value().result);
            auto res = outcome.value();
            const bool approximate = separationInfo.known() ? separationInfo.isApproximate()
                                                            : juce::String(res.method_used).equalsIgnoreCase("hpss");
            // An HPSS "vocals" is the whole harmonic mix: analysing it as the
            // vocal would feed a guess to every vocal panel (AID-07).
            if (!approximate) {
                AudioFileRef vocalFile;
                vocalFile.path = res.vocals_path;
                vocalFile.id = "vocals";
                AudioAnalyzer analyzer;
                auto analysisOutcome = analyzer.analyze(vocalFile);
                if (analysisOutcome.is_ok()) {
                    vocalRes = analysisOutcome.value();
                }
            }
        } else if (finalStatus.is_ok()) {
            outcome = khensu::Result<ipc::StemSeparationResult, ipc::BridgeError>::err(
                {ipc::BridgeError::Kind::HttpError, "Background separation failed: " + finalStatus.value().error});
        } else {
            // The wait ended without a finished job: keep why (could not
            // start, service lost, timed out, cancelled) - it used to be
            // replaced by a generic "did not complete".
            outcome = khensu::Result<ipc::StemSeparationResult, ipc::BridgeError>::err(finalStatus.error());
        }

        juce::MessageManager::callAsync([outcome, vocalRes, method_str, sessionId, separationGeneration, separationInfo,
                                         cb = std::move(cb), self]() mutable {
            if (self == nullptr) return; // service destroyed - drop callback safely
            const bool staleSession = self->separationGeneration_.load() != separationGeneration ||
                                      !self->sessions_.state().session ||
                                      self->sessions_.state().session->id != sessionId;
            // Only the run that set the flag may clear it: a stale completion
            // used to clear the NEXT session's in-flight separation (AUDIT-067).
            if (!staleSession) self->sessions_.state().separatingStems = false;
            if (staleSession) {
                cb(false, TRANS("Session changed before separation finished."));
                self->notifyVocalAnalysisListeners();
                return;
            }
            if (outcome.is_ok()) {
                auto res = outcome.value();
                self->sessions_.state().lastSeparationResult = res;
                if (vocalRes.has_value()) {
                    self->sessions_.state().vocalAnalysis = vocalRes;
                }
                juce::String usedMethod =
                    separationInfo.known() ? separationInfo.method : juce::String(res.method_used);
                // AID-07: the REAL fallback reason (separation.json) instead
                // of always "Demucs is not installed" - it is often a model
                // download that was blocked, a torch error, or a timeout.
                juce::String note;
                if (method_str == "roformer" && !usedMethod.equalsIgnoreCase("roformer")) {
                    // CE-AI-4: RoFormer fell back (Demucs, then HPSS).
                    note = "\n\n" + TRANS("Note: RoFormer separation was not used; these stems come from the "
                                          "fallback named above.");
                    if (separationInfo.reason.isNotEmpty())
                        note << "\n" << TRANS("Reason:") << " " << separationInfo.reason;
                } else if (method_str == "demucs" && usedMethod.equalsIgnoreCase("hpss")) {
                    note = "\n\n" + TRANS("Note: AI separation (Demucs) was not used, so these are approximate "
                                          "harmonic/percussive stems, not real separated instruments.");
                    if (separationInfo.reason.isNotEmpty())
                        note << "\n" << TRANS("Reason:") << " " << separationInfo.reason;
                }
                cb(true, TRANS("Stems separated") + " (" +
                             (usedMethod.isEmpty() ? juce::String(method_str) : usedMethod).toUpperCase() + "):\n" +
                             TRANS("Vocals:") + " " + juce::String(res.vocals_path) + "\n" + TRANS("Drums:") + " " +
                             juce::String(res.drums_path) + "\n" + TRANS("Bass:") + " " + juce::String(res.bass_path) +
                             "\n" + TRANS("Other:") + " " + juce::String(res.other_path) + note);
            } else {
                cb(false, TRANS("Stem separation failed:") + " " + juce::String(outcome.error().message));
            }
            self->notifyVocalAnalysisListeners();
        });
    });
}

// ---- Vocal-stem analysis ---------------------------------------------------------

bool StemService::hasVocalAnalysis() const noexcept { return sessions_.state().vocalAnalysis.has_value(); }

const AnalysisResult& StemService::currentVocalAnalysis() const noexcept { return *sessions_.state().vocalAnalysis; }

void StemService::clearVocalAnalysis() noexcept { sessions_.state().vocalAnalysis = std::nullopt; }

void StemService::ensureVocalAnalysis()
{
    if (sessions_.state().vocalAnalysis.has_value()) return;
    if (!hasStemsAvailable()) return;

    AudioFileRef vocalFile;
    vocalFile.path = getStemFile("vocals").getFullPathName().toStdString();
    vocalFile.id = "vocals";
    const auto generation = vocalAnalysisGeneration_.fetch_add(1) + 1;
    const std::string sessionId = sessions_.state().session ? sessions_.state().session->id : std::string{};
    const BackgroundReadKey readKey{sessionId, vocalFile.path, "vocals", generation};

    juce::WeakReference<StemService> self(this);
    work_.runInBackground([self, vocalFile, readKey]() {
        AudioAnalyzer analyzer;
        auto outcome = analyzer.analyze(vocalFile);
        if (outcome.is_err()) return;

        juce::MessageManager::callAsync([self, result = outcome.value(), readKey]() {
            if (self == nullptr) return;
            const std::string currentSessionId =
                self->sessions_.state().session ? self->sessions_.state().session->id : std::string{};
            const BackgroundReadKey currentKey{currentSessionId,
                                               self->sessions_.state().session
                                                   ? self->getStemFile("vocals").getFullPathName().toStdString()
                                                   : std::string{},
                                               "vocals", self->vocalAnalysisGeneration_.load()};
            if (!readKey.matches(currentKey)) return;
            if (self->sessions_.state().vocalAnalysis.has_value()) return; // a real run already beat us to it
            self->sessions_.state().vocalAnalysis = result;
            self->notifyVocalAnalysisListeners();
        });
    });
}

// ---- Session reset ---------------------------------------------------------------

void StemService::resetForSession()
{
    sessions_.state().soloedStemName.clear();
    sessions_.state().lastSeparationResult.reset();
    sessions_.state().vocalAnalysis.reset();
    // An in-flight separation of the old session stops polling and cancels
    // its job; the new session starts with no separation running (AUDIT-067).
    separationGeneration_.fetch_add(1);
    sessions_.state().separatingStems = false;
    editedVocal_.clear();
    notifyCurrentVocal();
}

juce::String StemService::currentVocalStem() const
{
    return editedVocal_.isNotEmpty() && juce::File(editedVocal_).existsAsFile() ? editedVocal_ : juce::String();
}

void StemService::notifyCurrentVocal()
{
    const auto listeners = currentVocalListeners_; // a listener may deregister itself
    for (const auto& [key, cb] : listeners)
        if (currentVocalListeners_.count(key) != 0 && cb) cb();
}

void StemService::setCurrentVocalStem(const juce::String& path)
{
    if (path == editedVocal_) return;
    editedVocal_ = path;
    notifyCurrentVocal();
    if (onCurrentVocalChanged) onCurrentVocalChanged();
}

void StemService::restoreCurrentVocalStem(const juce::String& path)
{
    editedVocal_ = juce::File(path).existsAsFile() ? path : juce::String();
    notifyCurrentVocal();
}

} // namespace khensu
