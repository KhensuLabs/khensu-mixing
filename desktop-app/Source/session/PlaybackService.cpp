// desktop-app/Source/session/PlaybackService.cpp
// Moved from AppController_Playback.cpp (ENG-1 stage C); behaviour unchanged.
#include "PlaybackService.h"
#include "SessionService.h"
#include "RenderService.h" // nativeChain (STU-9)
#include "../AbPreferenceMath.h"
#include "../BackgroundReadKey.h"
#include "../IBackgroundWork.h"
#include "../services/IAnalysisHub.h"
#include "../services/IStems.h"
#include "../ui/shared/BackgroundStop.h"
#include <khensu/analysis/AudioAnalyzer.h>
#include <khensu/analysis/LoudnessAnalyzer.h>
#include <khensu/feedback/FeedbackManager.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/session/AudioFileProber.h>
#include <cmath>

namespace khensu {
namespace {
/// Integrated LUFS of a file via the shared BS.1770 meter (LoudnessAnalyzer) -
/// only the loudness pass, not a full AudioAnalyzer analysis, so a B render
/// is matched (and unmuted) as quickly as the file can be decoded.
std::optional<float> measureIntegratedLufs(const juce::File& file)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader = AudioFileProber::openReader(fm, file);
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0) return std::nullopt;
    const int nCh = juce::jlimit(1, LoudnessAnalyzer::kMaxChannels, (int)reader->numChannels);
    LoudnessAnalyzer meter;
    meter.reset(reader->sampleRate, nCh);
    constexpr int kBlock = 8192;
    juce::AudioBuffer<float> buf(nCh, kBlock);
    for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += kBlock) {
        if (backgroundShouldStop()) return std::nullopt;
        const int n = (int)juce::jmin((juce::int64)kBlock, reader->lengthInSamples - pos);
        reader->read(&buf, 0, n, pos, true, true);
        const float* channels[LoudnessAnalyzer::kMaxChannels] = {};
        for (int ch = 0; ch < nCh; ++ch)
            channels[ch] = buf.getReadPointer(ch);
        meter.process(channels, nCh, n);
    }
    const float lufs = meter.getResult().lufs_integrated;
    // -70 is the meter's "nothing above the absolute gate" floor: not a level
    // to match against.
    if (!std::isfinite(lufs) || lufs <= -70.0f) return std::nullopt;
    return lufs;
}

std::string sessionIdOf(const SessionService& sessions)
{
    return sessions.active() ? sessions.active()->id : std::string{};
}
} // namespace

PlaybackService::PlaybackService(IBackgroundWork& work, SessionService& sessions, IStems& stems, IAnalysisHub& analysis)
    : work_(work), sessions_(sessions), stems_(stems), analysis_(analysis)
{}

PlaybackService::~PlaybackService() = default;

// ---- Transport ---------------------------------------------------------------

void PlaybackService::playAudio() { engine_.play(); }
void PlaybackService::pauseAudio() { engine_.pause(); }
void PlaybackService::stopAudio() { engine_.stop(); }

void PlaybackService::setPlaybackPosition(double seconds)
{
    engine_.setPosition(seconds);
    seekDaw(static_cast<float>(seconds));
}

double PlaybackService::getPlaybackPosition() const { return engine_.getPosition(); }
double PlaybackService::getPlaybackDuration() const { return engine_.getLength(); }
bool PlaybackService::isPlaying() const { return engine_.isPlaying(); }

void PlaybackService::seekDaw(float seconds)
{
    work_.runInBackground([seconds]() {
        ipc::HttpServiceBridge bridge;
        bridge.seekTransport(seconds);
    });
}

void PlaybackService::loadAudioFileForPlayback(const juce::File& file)
{
    // One file on one deck: any A/B pair is let go (UI-6).
    pairA_.clear();
    pairB_.clear();
    loadedPath_ = file.getFullPathName().toStdString();
    engine_.clearSource();
    if (file.existsAsFile() && engine_.loadFile(file) > 0.0)
        startWaveformScan(file);
    else
        startWaveformScan({}); // clears the peaks
    updatePlaybackGain();
}

void PlaybackService::startWaveformScan(const juce::File& file)
{
    const auto waveformGeneration = waveformGeneration_.fetch_add(1) + 1;
    const BackgroundReadKey readKey{sessionIdOf(sessions_), loadedPath_, "waveform", waveformGeneration};
    {
        const juce::ScopedLock sl(waveformLock_);
        waveformPeaks_.clear();
    }
    if (file.existsAsFile()) {
        // Downsampled waveform peaks, off the message thread. `this` is safe
        // on the worker: the owner drains the pool before destroying us.
        juce::WeakReference<PlaybackService> self(this);
        work_.runInBackground([this, self, file, readKey]() {
            std::vector<float> peaks;
            if (auto* r = engine_.createReaderFor(file)) {
                const int numPeaks = 500;
                peaks.resize(numPeaks, 0.0f);

                const int64_t totalSamples = r->lengthInSamples;
                const int64_t samplesPerPixel = totalSamples / numPeaks;

                if (samplesPerPixel > 0) {
                    const int numChannels = r->numChannels;
                    juce::AudioBuffer<float> buffer(numChannels, 1024);

                    for (int i = 0; i < numPeaks; ++i) {
                        // A newer file was loaded (quick switching queued
                        // several full decodes) or the app is quitting.
                        if ((i & 15) == 0 &&
                            (waveformGeneration_.load() != readKey.generation || backgroundShouldStop())) {
                            delete r;
                            return;
                        }
                        int64_t startSample = i * samplesPerPixel;
                        int64_t endSample = juce::jmin(totalSamples, startSample + samplesPerPixel);

                        float maxVal = 0.0f;
                        int64_t pos = startSample;
                        while (pos < endSample) {
                            int numToRead = (int)juce::jmin((int64_t)1024, endSample - pos);
                            r->read(&buffer, 0, numToRead, pos, true, true);

                            for (int ch = 0; ch < numChannels; ++ch) {
                                auto* data = buffer.getReadPointer(ch);
                                for (int s = 0; s < numToRead; ++s) {
                                    float val = std::abs(data[s]);
                                    if (val > maxVal) maxVal = val;
                                }
                            }
                            pos += numToRead;
                        }
                        peaks[i] = maxVal;
                    }
                }
                delete r;
            }

            juce::MessageManager::callAsync([self, readKey, peaks = std::move(peaks)]() mutable {
                auto* service = self.get();
                if (service == nullptr) return;
                const BackgroundReadKey currentKey{sessionIdOf(service->sessions_), service->loadedPath_, "waveform",
                                                   service->waveformGeneration_.load()};
                if (!readKey.matches(currentKey)) return;
                {
                    const juce::ScopedLock sl(service->waveformLock_);
                    service->waveformPeaks_ = std::move(peaks);
                }
                if (service->onWaveformChanged) service->onWaveformChanged();
            });
        });
    }
}

std::vector<float> PlaybackService::getWaveformPeaks() const
{
    const juce::ScopedLock sl(waveformLock_);
    return waveformPeaks_;
}

// ---- Gain --------------------------------------------------------------------

void PlaybackService::setPlaybackVolume(float gain01) noexcept
{
    source_.userVolume = juce::jlimit(0.0f, 1.0f, gain01);
    updatePlaybackGain();
}

void PlaybackService::updatePlaybackGain() noexcept
{
    // Matching rules live in PlaybackSourceController::gainFor (unit-tested).
    // Until B is measured it is held muted - never matched against a
    // genre-target guess (AUDIT-038).
    PlaybackSourceController::GainContext ctx;
    if (analysis_.hasAnalysis()) ctx.originalLufs = analysis_.currentAnalysis().loudness.lufs_integrated;
    ctx.originalPath = sessions_.activeSourcePath(); // gainFor swaps in a pinned A (STU-8)
    if (abPairLoaded()) {
        // UI-6: both decks are live - each gets its own matched gain, so
        // the crossfade moves between two levelled signals.
        ctx.loadedPath = pairA_;
        engine_.setGain(source_.gainFor(ctx));
        ctx.loadedPath = pairB_;
        engine_.setAlternateGain(source_.gainFor(ctx));
        return;
    }
    ctx.loadedPath = juce::String(loadedPath_);
    engine_.setGain(source_.gainFor(ctx));
}

AbLevelOffset PlaybackService::abLevelOffset() const
{
    std::optional<float> originalLufs;
    if (analysis_.hasAnalysis()) originalLufs = analysis_.currentAnalysis().loudness.lufs_integrated;
    return source_.levelOffset(originalLufs);
}

bool PlaybackService::abPairLoaded() const
{
    return engine_.hasAlternate() && pairB_.isNotEmpty() && pairB_ == source_.processedPath
           && pairA_ == aSidePath();
}

bool PlaybackService::loadAbPair()
{
    const juce::String original = aSidePath(); // STU-8: a pinned A, else the original
    const juce::String processed = source_.processedPath;
    loadAudioFileForPlayback(juce::File(original)); // main deck = A (clears any old pair)
    if (processed.isEmpty() || engine_.loadAlternate(juce::File(processed)) <= 0.0) return false;
    pairA_ = original;
    pairB_ = processed;
    return true;
}

void PlaybackService::setStreamingPreviewGainDb(float offsetDb) noexcept
{
    source_.streamingPreviewOffsetDb = offsetDb;
    updatePlaybackGain();
}

void PlaybackService::auditionLevelMatchedPreview(const juce::File& previewFile)
{
    if (!previewFile.existsAsFile()) return;
    const juce::String path = previewFile.getFullPathName();
    source_.previewPath = path;
    source_.previewLufs = lufsCache_.find(path);
    source_.previewLufsUnavailable = false;

    const double pos = engine_.getPosition();
    const bool playing = engine_.isPlaying();
    loadAudioFileForPlayback(previewFile); // applies the (muted-until-measured) gain
    engine_.setPosition(pos);
    if (playing) engine_.play();

    if (source_.previewLufs.has_value()) return;
    juce::WeakReference<PlaybackService> self(this);
    work_.runInBackground([self, path]() {
        const auto lufs = measureIntegratedLufs(juce::File(path));
        juce::MessageManager::callAsync([self, path, lufs]() {
            auto* service = self.get();
            if (service == nullptr || service->source_.previewPath != path) return;
            if (lufs.has_value()) {
                service->lufsCache_.remember(path, *lufs);
                service->source_.previewLufs = *lufs;
            } else {
                service->source_.previewLufsUnavailable = true;
            }
            service->updatePlaybackGain();
        });
    });
}

// ---- Stem solo ---------------------------------------------------------------

juce::String PlaybackService::currentStemSolo() const noexcept { return sessions_.state().soloedStemName; }

void PlaybackService::soloStem(const juce::String& stemName)
{
    if (!sessions_.hasSession() || sessions_.active()->files.empty()) return;

    // Empty name = back to the full mix - on the A/B side the indicator
    // shows (DSK-14: un-solo always loaded A, while the strip still said
    // "Processed").
    const bool wantB = source_.showingProcessed && hasAbComparison();
    const juce::File target = stemName.isEmpty()
                                  ? juce::File(wantB ? source_.processedPath : aSidePath())
                                  : stems_.getStemFile(stemName);
    if (!target.existsAsFile()) return;

    const double pos = engine_.getPosition();
    const bool playing = engine_.isPlaying();

    sessions_.state().soloedStemName = stemName;
    loadAudioFileForPlayback(target);
    engine_.setPosition(pos);
    if (playing) engine_.play();

    // Re-analyze whatever is now soloed so panels showing "what you're
    // currently hearing" (vocal quick stats, vocal analysis tab) actually
    // change when the user switches stems, instead of staying frozen on
    // whatever was last pushed from the full-mix analysis.
    const std::string targetPath = target.getFullPathName().toStdString();
    const std::string label = stemName.isEmpty() ? "mix" : stemName.toStdString();
    const auto generation = stemAnalysisGeneration_.fetch_add(1) + 1;
    const BackgroundReadKey readKey{sessionIdOf(sessions_), targetPath, label, generation};
    juce::WeakReference<PlaybackService> self(this);
    work_.runInBackground([targetPath, label, readKey, self]() {
        AudioFileRef ref;
        ref.path = targetPath;
        ref.id = label;
        AudioAnalyzer analyzer;
        auto outcome = analyzer.analyze(ref);
        if (outcome.is_err()) return;

        juce::MessageManager::callAsync([outcome, readKey, self]() {
            auto* service = self.get();
            if (service == nullptr) return;
            const auto solo = service->currentStemSolo();
            const bool mixSelected = solo.isEmpty();
            const auto* session = service->sessions_.active();
            const std::string currentPath =
                session != nullptr && !session->files.empty()
                    ? (mixSelected ? juce::File(service->sessions_.activeSourcePath()).getFullPathName().toStdString()
                                   : service->stems_.getStemFile(solo).getFullPathName().toStdString())
                    : std::string{};
            const BackgroundReadKey currentKey{sessionIdOf(service->sessions_), currentPath,
                                               mixSelected ? "mix" : solo.toStdString(),
                                               service->stemAnalysisGeneration_.load()};
            if (!readKey.matches(currentKey)) return;
            notify(service->stemAnalysisListeners_, outcome.value());
        });
    });
}

// ---- Loudness-matched A/B ----------------------------------------------------

bool PlaybackService::hasAbComparison() const noexcept
{
    return !source_.processedPath.isEmpty() && juce::File(source_.processedPath).existsAsFile();
}

void PlaybackService::learnFromAbPreference(bool preferredProcessed, bool bIsBlindReference)
{
    // DSK-07: the A/B is loudness-MATCHED (B is attenuated to A's level, or
    // muted until measured), so the pick carries no loudness information -
    // nudging the LUFS target from it taught the user's "taste" for a
    // loudness they never heard. What it does say is whether the PROCESSING
    // won at equal loudness: that is what is learned. A blind third-party
    // reference teaches nothing about the user's own processing at all.
    const auto decision = decideAbPreferenceLearning(source_.processedPath.isNotEmpty(), bIsBlindReference);
    if (!decision.recordProcessingPreference || !feedback_ || !sessions_.hasSession()) return;

    // Off the message thread (DSK-20): two small SQLite writes under WAL.
    std::shared_ptr<FeedbackManager> fm = feedback_;
    work_.runInBackground([fm, preferredProcessed] {
        const float before = fm->getPreference(kAbProcessingPreferenceKey, 0.0f).value_or(0.0f);
        fm->setPreference(kAbProcessingPreferenceKey, nudgeProcessingPreference(before, preferredProcessed));
        const float picks = fm->getPreference(kAbProcessingPicksKey, 0.0f).value_or(0.0f);
        fm->setPreference(kAbProcessingPicksKey, picks + 1.0f);
    });
}

void PlaybackService::primeAbLoudness(const juce::StringArray& paths, std::function<void(bool allKnown)> onDone)
{
    juce::StringArray todo;
    for (const auto& p : paths)
        if (p.isNotEmpty() && !lufsCache_.find(p).has_value()) todo.addIfNotAlreadyThere(p);
    if (todo.isEmpty()) {
        if (onDone) onDone(true);
        return;
    }
    juce::WeakReference<PlaybackService> self(this);
    work_.runInBackground([self, todo, onDone]() {
        std::vector<std::pair<juce::String, std::optional<float>>> measured;
        for (const auto& p : todo) {
            if (backgroundShouldStop()) return;
            measured.emplace_back(p, measureIntegratedLufs(juce::File(p)));
        }
        juce::MessageManager::callAsync([self, measured, onDone]() {
            auto* service = self.get();
            if (service == nullptr) return;
            bool allKnown = true;
            for (const auto& [path, lufs] : measured) {
                if (lufs.has_value())
                    service->rememberLufs(path, *lufs);
                else
                    allKnown = false; // unmeasurable: it plays unmatched rather than muted
            }
            if (onDone) onDone(allKnown);
        });
    });
}

void PlaybackService::rememberLufs(const juce::String& path, float lufs)
{
    lufsCache_.remember(path, lufs);
    // The B side may be waiting on exactly this measurement.
    if (source_.processedPath == path && !source_.processedLufs.has_value()) {
        source_.processedLufs = lufs;
        source_.processedLufsUnavailable = false;
        updatePlaybackGain();
    }
    // ... or a pinned A (STU-8).
    if (source_.referencePath == path && !source_.referenceLufs.has_value()) {
        source_.referenceLufs = lufs;
        source_.referenceLufsUnavailable = false;
        updatePlaybackGain();
    }
}

juce::String PlaybackService::aSidePath() const { return source_.aSidePath(sessions_.activeSourcePath()); }

void PlaybackService::setAbReferencePath(const juce::String& path)
{
    // The original pinned as A is the default A: nothing is pinned.
    const juce::String pin = path == sessions_.activeSourcePath() ? juce::String() : path;
    if (pin == source_.referencePath) return;
    if (pin.isNotEmpty() && !juce::File(pin).existsAsFile()) return;
    if (source_.assignReference(pin, lufsCache_.find(pin))) measureAbReferenceLufs(pin);

    // Reload what is audible so A is the pinned file: the pair (A changed
    // under it) or, on A without a B, the single deck.
    if (sessions_.hasSession() && !sessions_.active()->files.empty()) {
        pairA_.clear(); // the loaded pair holds the old A
        setAbSide(source_.showingProcessed);
    }
    updatePlaybackGain();
}

void PlaybackService::measureAbReferenceLufs(const juce::String& path)
{
    juce::WeakReference<PlaybackService> self(this);
    work_.runInBackground([path, self]() {
        const auto lufs = measureIntegratedLufs(juce::File(path));
        juce::MessageManager::callAsync([self, path, lufs]() {
            auto* service = self.get();
            if (service == nullptr || service->source_.referencePath != path) return; // re-pinned meanwhile
            if (lufs.has_value())
                service->rememberLufs(path, *lufs);
            else
                service->source_.referenceLufsUnavailable = true; // plays unmatched, not muted forever
            service->updatePlaybackGain();
        });
    });
}

void PlaybackService::measureAbProcessedLufs(const juce::String& path)
{
    juce::WeakReference<PlaybackService> self(this);
    work_.runInBackground([path, self]() {
        const auto lufs = measureIntegratedLufs(juce::File(path));
        juce::MessageManager::callAsync([self, path, lufs]() {
            auto* service = self.get();
            if (service == nullptr) return;
            // Ignore a stale result if the B source changed while measuring.
            if (service->source_.processedPath != path) return;
            if (lufs.has_value()) {
                service->rememberLufs(path, *lufs);
                service->source_.processedLufs = *lufs;
            } else {
                // Unmeasurable (silent / undecodable): play it unmatched
                // rather than holding B muted forever.
                service->source_.processedLufsUnavailable = true;
            }
            service->updatePlaybackGain();
        });
    });
}

void PlaybackService::setAbProcessedPath(const juce::String& path) noexcept
{
    const bool exists = path.isNotEmpty() && juce::File(path).existsAsFile();
    const bool pairWasLoaded = abPairLoaded();
    const auto todo = source_.assignProcessed(path, juce::String(loadedPath_), lufsCache_.find(path), exists);
    if (todo.measure) measureAbProcessedLufs(path);

    if (pairWasLoaded) {
        // The alternate deck holds the old B - or this same path, rendered
        // again in place (AUDIT-018): it is stale either way.
        pairB_.clear();
        if (source_.showingProcessed && exists) {
            setAbSide(true); // reload the pair with the new B, still on B
        } else {
            engine_.clearAlternate(); // crossfades back to A if B was audible
            const juce::String a = pairA_;
            pairA_.clear();
            if (juce::String(loadedPath_) != a) {
                loadedPath_ = a.toStdString();
                startWaveformScan(juce::File(a));
            }
            updatePlaybackGain();
        }
    } else if (todo.reloadTransport) {
        // B is what the transport is playing: a new B - or the SAME path
        // re-rendered in place (vocal macro preview, re-applied fix) - must
        // be reloaded, or the user keeps hearing stale / torn audio
        // (AUDIT-018). An empty / missing B falls back to A.
        setAbSide(exists);
    } else {
        updatePlaybackGain(); // B muted until measured
    }

    // A processed render just became available - let subscribers (e.g. the
    // Compression X-Ray panel) auto-refresh against it (spec §1.2/B).
    if (exists) notify(abRenderAppliedListeners_);
}

void PlaybackService::setAbSide(bool showProcessed)
{
    if (!sessions_.hasSession() || sessions_.active()->files.empty()) return;
    const bool wantB = showProcessed && hasAbComparison();

    const double pos = engine_.getPosition();
    const bool playing = engine_.isPlaying();

    source_.showingProcessed = wantB;

    // UI-6: with a B to compare, A and B play together and the switch is an
    // equal-power crossfade (~50 ms) - no fade-out, reload and fade-in.
    if (hasAbComparison()) {
        if (!abPairLoaded()) {
            if (loadAbPair()) {
                engine_.setPosition(pos);
                engine_.crossfadeTo(wantB); // not playing yet: immediate
                updatePlaybackGain();
                if (playing) engine_.play();
            } else {
                // B would not load beside A: the single-deck switch.
                loadAudioFileForPlayback(juce::File(wantB ? source_.processedPath : aSidePath()));
                engine_.setPosition(pos);
                if (playing) engine_.play();
                return;
            }
        } else {
            engine_.crossfadeTo(wantB);
        }
        const juce::String target = wantB ? pairB_ : pairA_;
        if (juce::String(loadedPath_) != target) {
            loadedPath_ = target.toStdString();
            startWaveformScan(juce::File(target));
        }
        updatePlaybackGain();
        return;
    }

    const juce::String target = wantB ? source_.processedPath : aSidePath();
    loadAudioFileForPlayback(juce::File(target));
    updatePlaybackGain();
    engine_.setPosition(pos); // seamless - same position
    if (playing) engine_.play();
}

void PlaybackService::toggleAb()
{
    if (!hasAbComparison() || !sessions_.hasSession() || sessions_.active()->files.empty()) return;
    setAbSide(!source_.showingProcessed);
}

void PlaybackService::loadAcceptedRender(const juce::String& renderedPathOrEmpty)
{
    if (!sessions_.hasSession() || sessions_.active()->files.empty()) return;

    const double pos = engine_.getPosition();
    const bool playing = engine_.isPlaying();

    const bool onOriginal = renderedPathOrEmpty.isEmpty();
    const juce::String target = onOriginal ? sessions_.activeSourcePath() : renderedPathOrEmpty;

    // Keep the A/B switch consistent with the render history: the "B" side
    // always points at whatever render is current. Same assignment +
    // cached/background LUFS levelling as setAbProcessedPath, minus its
    // transport reload - the single load below does that (it used to load
    // twice, each with its own fade).
    if (!onOriginal) {
        const bool exists = juce::File(target).existsAsFile();
        if (source_.assignProcessed(target, {}, lufsCache_.find(target), exists).measure)
            measureAbProcessedLufs(target);
        if (exists) notify(abRenderAppliedListeners_);
        source_.showingProcessed = true;
    } else {
        source_.showingProcessed = false;
    }

    loadAudioFileForPlayback(juce::File(target));
    engine_.setPosition(pos);
    if (playing) engine_.play();
}

// ---- Meters and session lifecycle --------------------------------------------

void PlaybackService::publishMeters(float lp, float rp, float momLufs, float stLufs, float corr, float width)
{
    notify(meterListeners_, lp, rp, momLufs, stLufs, corr, width);
}

void PlaybackService::invalidateReads() noexcept
{
    waveformGeneration_.fetch_add(1);
    stemAnalysisGeneration_.fetch_add(1);
}

void PlaybackService::resetForSession()
{
    source_.resetForSession();
    clearLivePreview(nullptr); // STU-9: an edit of the old track is not previewed on the new one
    pairA_.clear();
    pairB_.clear();
    unloadAudioFileForPlayback();
    waveformGeneration_.fetch_add(1); // an in-flight peak scan of the old file is dropped
    loadedPath_.clear();
    {
        const juce::ScopedLock sl(waveformLock_);
        waveformPeaks_.clear();
    }
}

// ---- STU-5: monitoring ------------------------------------------------------------

dsp::MonitorSolo::Mode PlaybackService::toSoloMode(MonitorMode mode) noexcept
{
    switch (mode) {
        case MonitorMode::Mono: return dsp::MonitorSolo::Mode::Mono;
        case MonitorMode::Side: return dsp::MonitorSolo::Mode::Side;
        case MonitorMode::Sub: return dsp::MonitorSolo::Mode::Sub;
        case MonitorMode::Full: break;
    }
    return dsp::MonitorSolo::Mode::Off;
}

IPlayback::MonitorMode PlaybackService::monitorMode() const noexcept
{
    switch (engine_.monitorMode()) {
        case dsp::MonitorSolo::Mode::Mono: return MonitorMode::Mono;
        case dsp::MonitorSolo::Mode::Side: return MonitorMode::Side;
        case dsp::MonitorSolo::Mode::Sub: return MonitorMode::Sub;
        default: break; // the band solos are the plugin's only
    }
    return MonitorMode::Full;
}

// ---- STU-9: live preview -----------------------------------------------------------

IPlayback::LivePreviewStatus PlaybackService::describePreview(const std::vector<dsp::RealtimeChain::StagePlan>& plan,
                                                              const juce::String& label)
{
    LivePreviewStatus status;
    status.active = true;
    status.label = label;
    for (const auto& stage : plan) {
        const juce::String name(stage.effect);
        switch (stage.mode) {
            case dsp::RealtimeChain::StageMode::Exact: status.live.add(name); break;
            case dsp::RealtimeChain::StageMode::CausalEquivalent:
                status.substituted.add(name + ": " + juce::String(stage.note));
                break;
            case dsp::RealtimeChain::StageMode::Skipped:
                status.skipped.add(name + ": " + juce::String(stage.note));
                break;
        }
    }
    return status;
}

void PlaybackService::setLivePreviewChain(void* owner, const std::vector<ipc::HttpServiceBridge::EffectSpec>& chain,
                                          const juce::String& label)
{
    previewOwner_ = owner;
    previewLabel_ = label;
    engine_.livePreview().setChain(RenderService::nativeChain(chain));
}

void PlaybackService::clearLivePreview(void* owner)
{
    if (owner != nullptr && owner != previewOwner_) return; // someone else's preview
    previewOwner_ = nullptr;
    previewLabel_.clear();
    engine_.livePreview().clear();
}

IPlayback::LivePreviewStatus PlaybackService::livePreviewStatus() const
{
    const auto& stage = engine_.livePreview();
    if (!stage.isEnabled()) return {};
    return describePreview(stage.plan(), previewLabel_);
}

} // namespace khensu
