#include "KhensuAraDocumentController.h"

#if JucePlugin_Enable_ARA

#include "../CopilotAnalysisController.h"
#include "../live/WindowIssueScanner.h"

#include <khensu/analysis/AudioAnalyzer.h>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/session/AudioFileRef.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/targets/GenreEntry.h>

#include <algorithm>

namespace khensu::ara {

namespace {

constexpr int kArchiveVersion = 1;
constexpr int kCancelWaitMs = 5000;

juce::String nameOf(const char* utf8, const juce::String& fallback)
{
    return utf8 != nullptr && *utf8 != 0 ? juce::String(juce::CharPointer_UTF8(utf8)) : fallback;
}

} // namespace

/// The job's output, read by the controller's timer on the message thread.
struct KhensuAraDocumentController::SharedResult {
    std::mutex mutex;
    std::vector<timeline::TimedIssue> issues; ///< source time
    juce::String status;
    bool analysed = false; ///< true when the source was measured (status is informational)
    std::atomic<bool> finished{false};
    std::atomic<bool> cancelled{false};
};

/// Reads one audio source and analyses it. Runs on the controller's pool; it
/// borrows the reader, which the controller owns and destroys on the message
/// thread only after this job has left the pool.
class KhensuAraDocumentController::AnalysisJob final : public juce::ThreadPoolJob {
  public:
    AnalysisJob(juce::ARAAudioSourceReader& reader, std::shared_ptr<SharedResult> result, juce::String name)
        : juce::ThreadPoolJob("khensu-ara-analysis"), reader_(reader), result_(std::move(result)),
          name_(std::move(name))
    {
    }

    JobStatus runJob() override
    {
        const auto finish = [this](std::vector<timeline::TimedIssue> issues, const juce::String& status,
                                   bool analysed = false) {
            {
                const std::lock_guard<std::mutex> lock(result_->mutex);
                result_->issues = std::move(issues);
                result_->status = status;
                result_->analysed = analysed;
            }
            result_->finished.store(true, std::memory_order_release);
            return jobHasFinished;
        };
        const auto cancelled = [this] {
            return shouldExit() || result_->cancelled.load(std::memory_order_relaxed);
        };

        const double sr = reader_.sampleRate;
        const int channels = static_cast<int>(juce::jlimit(1u, 8u, reader_.numChannels));
        const juce::int64 total = reader_.lengthInSamples;
        if (sr <= 0.0 || total <= 0) return finish({}, TRANS("No audio in this source"));
        const juce::int64 limit = std::min<juce::int64>(total, static_cast<juce::int64>(kMaxAnalysedSeconds * sr));

        // 1) Read through the host (never on the audio thread), measuring in
        //    windows and writing the temp WAV the service will analyse.
        const auto tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("KhensuARA");
        tempDir.createDirectory();
        const auto wavFile = tempDir.getNonexistentChildFile("source", ".wav");
        const juce::ScopeGuard removeTemp{[wavFile] { wavFile.deleteFile(); }};

        std::unique_ptr<juce::AudioFormatWriter> writer;
        {
            juce::WavAudioFormat wav;
            if (auto stream = std::make_unique<juce::FileOutputStream>(wavFile); stream->openedOk()) {
                const auto options = juce::AudioFormatWriterOptions{}
                                         .withSampleRate(sr)
                                         .withNumChannels(channels)
                                         .withBitsPerSample(24);
                std::unique_ptr<juce::OutputStream> out = std::move(stream);
                writer = wav.createWriterFor(out, options);
            }
        }
        if (writer == nullptr) return finish({}, TRANS("Could not write a temporary WAV for analysis"));

        live::WindowIssueScanner scanner(timeline::IssueSource::Ara, "ara");
        std::vector<timeline::TimedIssue> issues;
        const auto sink = [&issues](live::WindowIssueScanner::Window&& w) {
            for (auto& i : w.issues) issues.push_back(std::move(i));
        };

        juce::AudioBuffer<float> block(channels, kReadBlockSamples);
        for (juce::int64 pos = 0; pos < limit; pos += kReadBlockSamples) {
            if (cancelled()) return finish({}, TRANS("Analysis cancelled"));
            const int count = static_cast<int>(std::min<juce::int64>(kReadBlockSamples, limit - pos));
            if (!reader_.isValid() || !reader_.read(block.getArrayOfWritePointers(), channels, pos, count))
                return finish({}, TRANS("The host withdrew access to the audio"));
            const double t = static_cast<double>(pos) / sr;
            scanner.feed(block.getArrayOfReadPointers(), channels, count, t, sr, sink);
            if (!writer->writeFromAudioSampleBuffer(block, 0, count))
                return finish({}, TRANS("Could not write a temporary WAV for analysis"));
        }
        scanner.flush(sink);
        writer.reset(); // closes the file

        // 2) The whole source: shared-core analysis, the service's deep
        //    analysis through the plugin's client (cancellable job), then the
        //    recommenders. Cards with a time range land there; the rest span
        //    the analysed audio.
        AudioFileRef ref;
        ref.id = "ara";
        ref.path = wavFile.getFullPathName().toStdString();
        ref.format = "wav";
        AudioAnalyzer analyzer;
        auto analysed = analyzer.analyze(ref, {});
        if (cancelled()) return finish({}, TRANS("Analysis cancelled"));
        bool serviceRan = false;
        if (analysed.is_ok()) {
            auto analysis = std::move(analysed.value());
            serviceRan = CopilotAnalysisController::enrichFromService(analysis, ref.path, cancelled);
            if (cancelled()) return finish({}, TRANS("Analysis cancelled"));
            RecommendationEngine engine;
            const auto recs = engine.generate(analysis, GenreEntry{}, CreativeTarget{});
            const double lengthSec = static_cast<double>(limit) / sr;
            auto whole = timeline::issuesFromRecommendations(recs, 0.0, 0.0, lengthSec, timeline::IssueSource::Ara,
                                                             "ara", name_.toStdString());
            for (auto& i : whole) issues.push_back(std::move(i));
        }

        juce::String status = serviceRan ? TRANS("Analysed (with the Khensu service)")
                                         : TRANS("Analysed in the plug-in (Khensu service not reachable)");
        if (limit < total) status << " - " << TRANS("first 30 minutes only");
        return finish(std::move(issues), status, true);
    }

  private:
    juce::ARAAudioSourceReader& reader_;
    std::shared_ptr<SharedResult> result_;
    juce::String name_;
};

KhensuAraDocumentController::KhensuAraDocumentController(const ARA::PlugIn::PlugInEntry* entry,
                                                         const ARA::ARADocumentControllerHostInstance* instance)
    : juce::ARADocumentControllerSpecialisation(entry, instance)
{
    startTimerHz(4);
}

KhensuAraDocumentController::~KhensuAraDocumentController()
{
    stopTimer();
    for (auto& [source, record] : sources_)
        if (record.result) record.result->cancelled.store(true);
    // Every stage polls the cancel flag (reads are 64k samples, service polls
    // 100 ms), so this returns quickly; readers are destroyed afterwards.
    pool_.removeAllJobs(true, kCancelWaitMs);
    sources_.clear();
}

bool KhensuAraDocumentController::doRestoreObjectsFromStream(juce::ARAInputStream& input,
                                                              const juce::ARARestoreObjectsFilter*) noexcept
{
    // Findings are recomputed from the audio, never archived, so a project
    // always shows what its audio measures today.
    if (input.getNumBytesRemaining() >= 4) juce::ignoreUnused(input.readInt());
    return !input.failed();
}

bool KhensuAraDocumentController::doStoreObjectsToStream(juce::ARAOutputStream& output,
                                                          const juce::ARAStoreObjectsFilter*) noexcept
{
    return output.writeInt(kArchiveVersion);
}

juce::ARAPlaybackRenderer* KhensuAraDocumentController::doCreatePlaybackRenderer() noexcept
{
    return new KhensuAraPlaybackRenderer(getDocumentController());
}

void KhensuAraDocumentController::willBeginEditing(juce::ARADocument*) { editing_ = true; }

void KhensuAraDocumentController::didEndEditing(juce::ARADocument*)
{
    editing_ = false;
    startPending();
    bump(); // regions may have moved
}

void KhensuAraDocumentController::didEnableAudioSourceSamplesAccess(juce::ARAAudioSource* source, bool enable)
{
    if (enable)
        scheduleAnalysis(source);
    else
        cancelAnalysis(source, false);
}

void KhensuAraDocumentController::doUpdateAudioSourceContent(juce::ARAAudioSource* source,
                                                             juce::ARAContentUpdateScopes scopes)
{
    if (scopes.affectSamples() && source->isSampleAccessEnabled()) scheduleAnalysis(source);
}

void KhensuAraDocumentController::willDestroyAudioSource(juce::ARAAudioSource* source)
{
    cancelAnalysis(source, true);
    bump();
}

void KhensuAraDocumentController::didUpdatePlaybackRegionProperties(juce::ARAPlaybackRegion*) { bump(); }
void KhensuAraDocumentController::willDestroyPlaybackRegion(juce::ARAPlaybackRegion*) { bump(); }
void KhensuAraDocumentController::didAddPlaybackRegionToAudioModification(juce::ARAAudioModification*,
                                                                          juce::ARAPlaybackRegion*)
{
    bump();
}

void KhensuAraDocumentController::scheduleAnalysis(juce::ARAAudioSource* source)
{
    if (source == nullptr) return;
    cancelAnalysis(source, false);
    auto& record = sources_[source];
    record.pending = true;
    record.analysed = false;
    record.status = TRANS("Waiting to analyse");
    if (!editing_) startPending();
    bump();
}

void KhensuAraDocumentController::startPending()
{
    for (auto& [source, record] : sources_) {
        if (!record.pending || record.job != nullptr) continue;
        if (!source->isSampleAccessEnabled()) continue;
        record.pending = false;
        // The reader registers itself as a model listener, so it is created
        // (and later destroyed) here on the message thread.
        record.reader = std::make_unique<juce::ARAAudioSourceReader>(source);
        record.result = std::make_shared<SharedResult>();
        record.job = std::make_unique<AnalysisJob>(*record.reader, record.result,
                                                   nameOf(source->getName(), TRANS("Audio source")));
        record.status = TRANS("Analysing...");
        pool_.addJob(record.job.get(), false);
    }
}

void KhensuAraDocumentController::cancelAnalysis(juce::ARAAudioSource* source, bool forget)
{
    const auto it = sources_.find(source);
    if (it == sources_.end()) return;
    auto& record = it->second;
    if (record.job != nullptr) {
        record.result->cancelled.store(true);
        // Bounded: the job polls the flag between 64k-sample reads and inside
        // the service wait.
        if (!pool_.removeJob(record.job.get(), true, kCancelWaitMs)) {
            juce::Logger::writeToLog("Khensu ARA: an analysis job did not stop in time");
            // Keep job + reader alive (leaked into the pool's hands) rather
            // than free memory a running job still reads.
            record.job.release();
            record.reader.release();
        }
        record.job.reset();
        record.reader.reset();
        record.result.reset();
    }
    record.pending = false;
    if (forget)
        sources_.erase(it);
    else
        record.status = TRANS("Waiting for the host");
}

void KhensuAraDocumentController::timerCallback()
{
    bool changed = false;
    for (auto& [source, record] : sources_) {
        if (record.job == nullptr || record.result == nullptr) continue;
        if (!record.result->finished.load(std::memory_order_acquire)) continue;
        if (!pool_.removeJob(record.job.get(), true, 100)) continue; // still unwinding; next tick
        {
            const std::lock_guard<std::mutex> lock(record.result->mutex);
            record.issues = std::move(record.result->issues);
            record.status = record.result->status;
            record.analysed = record.result->analysed;
        }
        record.job.reset();
        record.reader.reset();
        record.result.reset();
        changed = true;
    }
    if (!editing_) startPending();
    if (changed) bump();
}

std::vector<timeline::TimedIssue> KhensuAraDocumentController::regionIssues()
{
    std::vector<timeline::TimedIssue> out;
    auto* document = getDocument();
    if (document == nullptr) return out;
    for (auto* source : document->getAudioSources<juce::ARAAudioSource>()) {
        const auto it = sources_.find(source);
        if (it == sources_.end() || it->second.issues.empty()) continue;
        const auto sourceName = nameOf(source->getName(), TRANS("Audio source"));
        for (auto* modification : source->getAudioModifications<juce::ARAAudioModification>()) {
            for (auto* region : modification->getPlaybackRegions<juce::ARAPlaybackRegion>()) {
                const auto regionName = nameOf(region->getName(), sourceName);
                auto mapped = timeline::mapSourceIssuesToRegion(
                    it->second.issues, region->getStartInAudioModificationTime(),
                    region->getDurationInAudioModificationTime(), region->getStartInPlaybackTime(),
                    regionName.toStdString());
                for (auto& i : mapped) out.push_back(std::move(i));
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.startSec < b.startSec; });
    return out;
}

bool KhensuAraDocumentController::requestPlaybackPosition(double timeSec)
{
    if (auto* controller = getDocumentController()->getHostPlaybackController()) {
        controller->requestSetPlaybackPosition(std::max(0.0, timeSec));
        return true;
    }
    return false;
}

juce::String KhensuAraDocumentController::statusText()
{
    int done = 0, running = 0, waiting = 0;
    juce::String lastProblem;
    for (const auto& [source, record] : sources_) {
        juce::ignoreUnused(source);
        if (record.job != nullptr)
            ++running;
        else if (record.pending)
            ++waiting;
        else if (record.analysed)
            ++done;
        else if (record.status.isNotEmpty())
            lastProblem = record.status;
    }
    if (sources_.empty()) return TRANS("ARA: no audio source shared by the host yet");
    juce::String s = "ARA: " + juce::String(done) + " " + TRANS("analysed");
    if (running > 0) s << ", " << running << " " << TRANS("analysing");
    if (waiting > 0) s << ", " << waiting << " " << TRANS("waiting");
    if (lastProblem.isNotEmpty()) s << " (" << lastProblem << ")";
    return s;
}

} // namespace khensu::ara

/// The ARA factory entry point JUCE's VST3 wrapper looks up.
const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<khensu::ara::KhensuAraDocumentController>();
}

#endif // JucePlugin_Enable_ARA
