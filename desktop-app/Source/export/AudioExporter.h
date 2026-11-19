// desktop-app/Source/export/AudioExporter.h
#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace khensu {

struct AudioExportOptions {
    enum class Format { WAV, AIFF, FLAC, MP3, AAC };
    enum class BitDepth { Bits16 = 16, Bits24 = 24, Bits32 = 32 };
    /// Source keeps the file's own rate - no resampling at all (UIX-21: the
    /// old 44.1k default sent every 48k session through ResamplingAudioSource,
    /// which is not mastering-grade).
    enum class SampleRate { Source = 0, SR44100 = 44100, SR48000 = 48000, SR96000 = 96000 };
    enum class Mp3Bitrate { kbps128 = 128, kbps192 = 192, kbps320 = 320 };

    juce::String sourcePath;
    juce::String outputPath;
    Format format = Format::WAV;
    BitDepth bitDepth = BitDepth::Bits24;
    SampleRate sampleRate = SampleRate::Source;
    Mp3Bitrate mp3Bitrate = Mp3Bitrate::kbps320;
    float gainLinear = 1.0f;
    std::atomic<bool>* cancelFlag = nullptr;   ///< optional; checked between write blocks
    std::atomic<float>* progressOut = nullptr; ///< optional; updated 0..1 between write blocks
    /// Owners of cancelFlag / progressOut. Copies of the options (worker
    /// lambdas) keep the atomics alive even if the UI that created them is
    /// destroyed mid-export, so the raw pointers above never dangle.
    std::shared_ptr<std::atomic<bool>> cancelFlagOwner;
    std::shared_ptr<std::atomic<float>> progressOwner;

    /// Whether `format` can be written at `depth` (AUDIT-041). JUCE's FLAC
    /// writer supports {16, 24} and its AIFF writer {8, 16, 24}, so 32-bit is
    /// WAV-only (written as IEEE float). Lossy formats ignore the depth (they
    /// are fed 16-bit PCM), so any depth is accepted for them.
    static constexpr bool supportsBitDepth(Format format, BitDepth depth) noexcept
    {
        if (format == Format::MP3 || format == Format::AAC) return true;
        return depth != BitDepth::Bits32 || format == Format::WAV;
    }

    /// The rate actually written. Source keeps `sourceRate`; MP3 (LAME) and
    /// AAC top out at 48 kHz, so a lossy export is capped there rather than
    /// failing in the encoder.
    static int resolveOutputRate(SampleRate requested, double sourceRate, bool lossy) noexcept
    {
        int rate = requested == SampleRate::Source ? static_cast<int>(sourceRate + 0.5) : static_cast<int>(requested);
        if (rate <= 0) rate = 44100;
        return lossy ? (rate < 48000 ? rate : 48000) : rate;
    }
};

// THREAD_CONTEXT(WorkerThread) - all methods block. Never call from UIThread or AudioThread.
class AudioExporter {
  public:
    AudioExporter() = delete;

    // Returns "" on success, error description on failure.
    static juce::String exportFile(const AudioExportOptions& opts);

    // Computes linear gain from measuredLufs -> targetLufs, clamped so the
    // output peak does not exceed ceilingDbtp. Pass truePeakDbtp from
    // AnalysisResult.loudness.true_peak_dbtp (the source's current peak).
    // ceilingDbtp defaults to 0.0 (bit-depth safety only) for callers that
    // don't have a platform-specific ceiling in hand; pass the target
    // platform's published ceiling (e.g. -1.0 for Spotify/Apple Music) to
    // actually enforce it rather than merely warning about it in the UI.
    static float computeGain(float measuredLufs, float targetLufs, float truePeakDbtp,
                             float ceilingDbtp = 0.0f) noexcept
    {
        const float gainLinear = std::pow(10.0f, (targetLufs - measuredLufs) / 20.0f);
        // Clamp: output true peak must not exceed ceilingDbtp (the platform's own
        // published ceiling when known, 0 dBTP bit-depth safety otherwise).
        const float peakLinear = std::pow(10.0f, truePeakDbtp / 20.0f);
        const float ceilingLinear = std::pow(10.0f, ceilingDbtp / 20.0f);
        const float maxGain = (peakLinear > 0.0f) ? (ceilingLinear / peakLinear) : 1.0f;
        return std::min(gainLinear, maxGain);
    }

    // True if lame.exe is found next to the application executable or in PATH.
    static bool isLameAvailable() noexcept;
};

} // namespace khensu
