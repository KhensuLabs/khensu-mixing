// THREAD_CONTEXT(WorkerThread)
#include <khensu/analysis/AudioAnalyzer.h>
#include <khensu/analysis/LoudnessAnalyzer.h>
#include <khensu/analysis/SpectralAnalyzer.h>
#include <khensu/analysis/DynamicsAnalyzer.h>
#include <khensu/analysis/StereoAnalyzer.h>
#include <khensu/analysis/ArtifactDetector.h>
#include <khensu/analysis/ChannelLayout.h>
#include <khensu/session/AudioFileProber.h>

#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace khensu {

Result<AnalysisResult> AudioAnalyzer::analyze(const AudioFileRef& fileRef, const std::string& sessionId)
{
    // Open the audio file
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats(); // WAV, AIFF, OGG, FLAC

    // The std::string overload decodes UTF-8; the const char* one is ASCII-only
    // and turned every non-ASCII path into an unopenable mojibake name.
    const juce::String pathStr(fileRef.path);
    const juce::File audioFile(pathStr);

    std::unique_ptr<juce::AudioFormatReader> reader = AudioFileProber::openReader(formatManager, audioFile);

    if (!reader) return Result<AnalysisResult>::err("Cannot open audio file: " + fileRef.path);

    const double sr = reader->sampleRate;
    const int ch = static_cast<int>(reader->numChannels);
    const int64_t total = static_cast<int64_t>(reader->lengthInSamples);

    if (sr <= 0.0 || sr > 384000.0 || total <= 0)
        return Result<AnalysisResult>::err("Invalid or out-of-bounds audio file parameters: " + fileRef.path);

    // Resolve the channel layout BEFORE any analyzer is constructed or any
    // audio is read. An unsupported layout must fail closed rather than produce
    // a confident-looking result measured from a subset of the channels: the
    // analyzers below silently capped at 8 channels and StereoAnalyzer read only
    // the first 2, so a 9+ channel file used to analyse "successfully" from
    // partial audio (audit AQ-01).
    const auto layoutResult = ChannelLayout::fromChannelCount(ch);
    if (layoutResult.is_err())
        return Result<AnalysisResult>::err(layoutResult.error() + " File: " + fileRef.path);
    const ChannelLayout layout = layoutResult.value();

    // Initialise all analyzers
    LoudnessAnalyzer loudness;
    SpectralAnalyzer spectral;
    DynamicsAnalyzer dynamics;
    StereoAnalyzer stereo;
    ArtifactDetector artifacts;

    loudness.reset(sr, layout);
    spectral.reset(sr, layout);
    dynamics.reset(sr, layout);
    stereo.reset(layout);
    artifacts.reset(sr);

    // Read and process in blocks
    juce::AudioBuffer<float> buffer(ch, kReadBlockSize);
    int64_t samplesRead = 0;

    while (samplesRead < total) {
        const int toRead = static_cast<int>(std::min(static_cast<int64_t>(kReadBlockSize), total - samplesRead));

        // A truncated/corrupt file or a mid-read I/O failure must not
        // silently continue as if nothing happened - the reader's own
        // success result was previously discarded, so a short/failed read
        // could still produce an apparently-successful analysis (audit CQ-05).
        if (!reader->read(&buffer, 0, toRead, samplesRead, true, true))
            return Result<AnalysisResult>::err(
                "Audio read failed partway through the file (truncated or corrupt data) at sample " +
                std::to_string(samplesRead) + ": " + fileRef.path);

        // Build non-const-inner-pointer array from JUCE's const float* const*
        std::vector<const float*> chanPtrs(ch);
        for (int c = 0; c < ch; ++c)
            chanPtrs[c] = buffer.getReadPointer(c);
        const float** ptrs = chanPtrs.data();

        for (int c = 0; c < ch; ++c) {
            const float* samples = ptrs[c];
            for (int i = 0; i < toRead; ++i) {
                if (!std::isfinite(samples[i]))
                    return Result<AnalysisResult>::err(
                        "Audio contains non-finite (NaN/Inf) samples starting near sample " +
                        std::to_string(samplesRead) + ": " + fileRef.path);
            }
        }

        loudness.process(ptrs, ch, toRead);
        spectral.process(ptrs, ch, toRead);
        dynamics.process(ptrs, ch, toRead);
        stereo.process(ptrs, ch, toRead);
        artifacts.process(ptrs, ch, toRead);

        samplesRead += toRead;
    }

    // Assemble result
    AnalysisResult result;
    result.id = juce::Uuid().toString().toStdString();
    result.session_id = sessionId;
    result.file_id = fileRef.id;
    result.computed_at = juce::Time::getCurrentTime().toISO8601(true).toStdString();
    result.is_partial = false;
    result.sample_rate = sr;
    result.channel_layout.layout_name = layout.name;
    result.channel_layout.channel_count = layout.numChannels;
    result.channel_layout.lfe_excluded = layout.hasLFE();

    result.loudness = loudness.getResult();
    result.spectral = spectral.getResult();
    result.dynamics = dynamics.getResult();
    result.stereo = stereo.getResult();
    result.phase = stereo.getPhaseResult();
    result.artifacts = artifacts.getResult();

    return Result<AnalysisResult>::ok(std::move(result));
}

} // namespace khensu
