// desktop-app/Source/export/AudioExporter.cpp
#include "AudioExporter.h"
#include <cmath>
#include <khensu/session/AudioFileProber.h>

namespace khensu {

namespace {
// TPDF dither: two independent uniform samples summed, scaled by 1 LSB.
// Dithering before integer truncation breaks quantisation noise correlations and
// pushes distortion below the noise floor. Apply only for 16-bit targets.
static void applyTpdfDither(juce::AudioBuffer<float>& buf, int numSamples, int numChannels, juce::Random& rng) noexcept
{
    constexpr float kLsb = 1.0f / 32768.0f; // 1 LSB amplitude at 16-bit
    for (int ch = 0; ch < numChannels; ++ch) {
        auto* p = buf.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i)
            p[i] += (rng.nextFloat() - rng.nextFloat()) * kLsb;
    }
}

// Locate the LAME encoder: first next to the app executable, then on PATH,
// then (macOS / Linux) the usual package-manager locations. Returns an empty
// File if not found anywhere. (CE-PLT-5: the name, PATH separator and the
// fallback directories are per platform.)
juce::File findLameExe() noexcept
{
#if JUCE_WINDOWS
    const juce::String exeName = "lame.exe";
    const juce::String pathSeparator = ";";
#else
    const juce::String exeName = "lame";
    const juce::String pathSeparator = ":";
#endif
    auto candidate = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile(exeName);
    if (candidate.existsAsFile()) return candidate;

    const auto path = juce::SystemStats::getEnvironmentVariable("PATH", "");
    for (const auto& dir : juce::StringArray::fromTokens(path, pathSeparator, "")) {
        auto f = juce::File(dir).getChildFile(exeName);
        if (f.existsAsFile()) return f;
    }
#if !JUCE_WINDOWS
    // A GUI app launched from Finder / the Dock gets a minimal PATH.
    for (const char* dir : {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin"}) {
        auto f = juce::File(dir).getChildFile(exeName);
        if (f.existsAsFile()) return f;
    }
#endif
    return {};
}

juce::LAMEEncoderAudioFormat makeLameFormat() { return juce::LAMEEncoderAudioFormat(findLameExe()); }

// Map Mp3Bitrate enum to the quality option index in LAMEEncoderAudioFormat.
// JUCE 7 LAME quality options: 0=32,1=40,2=48,3=56,4=64,5=80,6=96,7=112,
// 8=128,9=160,10=192,11=224,12=256,13=320
int lameQualityIndex(AudioExportOptions::Mp3Bitrate br) noexcept
{
    switch (br) {
    case AudioExportOptions::Mp3Bitrate::kbps128:
        return 8;
    case AudioExportOptions::Mp3Bitrate::kbps192:
        return 10;
    case AudioExportOptions::Mp3Bitrate::kbps320:
        return 13;
    }
    return 13;
}

juce::AudioFormatWriterOptions writerOptions(double sampleRate, int numChannels, int bitsPerSample,
                                             int qualityOptionIndex = 0)
{
    // 32-bit (WAV only) is IEEE float: preserves overs instead of clipping
    // them, and is what "32-bit" means to every DAW / distributor.
    return juce::AudioFormatWriterOptions{}
        .withSampleRate(sampleRate)
        .withNumChannels(numChannels)
        .withBitsPerSample(bitsPerSample)
        .withSampleFormat(bitsPerSample == 32 ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                              : juce::AudioFormatWriterOptions::SampleFormat::automatic)
        .withQualityOptionIndex(qualityOptionIndex);
}
} // anonymous namespace

// computeGain is inline in AudioExporter.h (pure math, unit-tested without the encoders).

bool AudioExporter::isLameAvailable() noexcept { return findLameExe().existsAsFile(); }

juce::String AudioExporter::exportFile(const AudioExportOptions& opts)
{
    juce::AudioFormatManager fmtMgr;
    fmtMgr.registerBasicFormats();

    // Validate before decoding anything: JUCE's FLAC/AIFF writers reject
    // 32-bit, which used to surface only as a vague "failed to create writer"
    // after the integrity pass (AUDIT-041).
    if (!AudioExportOptions::supportsBitDepth(opts.format, opts.bitDepth))
        return "32-bit export is only available for WAV (FLAC supports 16/24-bit, AIFF 8/16/24-bit). "
               "Choose 24-bit or switch the format to WAV.";

    juce::File srcFile(opts.sourcePath);
    if (!srcFile.existsAsFile()) return "Source file not found: " + opts.sourcePath;

    auto reader = AudioFileProber::openReader(fmtMgr, srcFile);
    if (!reader) return "Cannot decode source file: " + opts.sourcePath;

    if (reader->lengthInSamples <= 0) return "Cannot determine source length: " + opts.sourcePath;

    juce::File outFile(opts.outputPath);
    outFile.getParentDirectory().createDirectory();
    // Write to a sibling temp file and swap it in only on success:
    // File::createOutputStream() APPENDS to an existing file (re-exporting over
    // a file produced a corrupt, doubled file), and a failed or cancelled
    // export must not leave a truncated file at the target path. The temp
    // file is deleted on every early return.
    juce::TemporaryFile tmpOut(outFile, juce::TemporaryFile::useHiddenFile);

    // Honour the requested output rate - the UI has offered 44.1/48/96 kHz
    // since day one, but exports used to silently keep the source rate.
    const double srcRate = reader->sampleRate;
    const bool lossy = opts.format == AudioExportOptions::Format::MP3 || opts.format == AudioExportOptions::Format::AAC;
    // MP3 (LAME) and AAC (Media Foundation / CoreAudio) top out at 48 kHz; a
    // 96 kHz request made the encoder fail outright. Cap rather than fail.
    const int outSR = AudioExportOptions::resolveOutputRate(opts.sampleRate, srcRate, lossy);
    const int nCh = static_cast<int>(reader->numChannels);
    // Lossy encoders are fed 16-bit PCM regardless of the depth picker, so the
    // dither decision must use 16 (a "24-bit" MP3 was truncated undithered).
    const int bitD = lossy ? 16 : static_cast<int>(opts.bitDepth);
    const bool needsResample = std::abs(srcRate - static_cast<double>(outSR)) > 1.0;

    std::unique_ptr<juce::AudioFormatWriter> writer;

    if (opts.format == AudioExportOptions::Format::WAV) {
        juce::WavAudioFormat wavFmt;
        std::unique_ptr<juce::OutputStream> stream = tmpOut.getFile().createOutputStream();
        if (!stream) return "Cannot create output file: " + opts.outputPath;
        writer = wavFmt.createWriterFor(stream, writerOptions(outSR, nCh, bitD));
    } else if (opts.format == AudioExportOptions::Format::AIFF) {
        juce::AiffAudioFormat aiffFmt;
        std::unique_ptr<juce::OutputStream> stream = tmpOut.getFile().createOutputStream();
        if (!stream) return "Cannot create output file: " + opts.outputPath;
        writer = aiffFmt.createWriterFor(stream, writerOptions(outSR, nCh, bitD));
    } else if (opts.format == AudioExportOptions::Format::FLAC) {
        juce::FlacAudioFormat flacFmt;
        std::unique_ptr<juce::OutputStream> stream = tmpOut.getFile().createOutputStream();
        if (!stream) return "Cannot create output file: " + opts.outputPath;
        writer = flacFmt.createWriterFor(stream, writerOptions(outSR, nCh, bitD));
    } else if (opts.format == AudioExportOptions::Format::MP3) {
        auto lameExe = findLameExe();
        if (!lameExe.existsAsFile()) return "LAME encoder not available. Place lame.exe next to the app or in PATH.";
        juce::LAMEEncoderAudioFormat lame(lameExe);
        std::unique_ptr<juce::OutputStream> stream = tmpOut.getFile().createOutputStream();
        if (!stream) return "Cannot create output file: " + opts.outputPath;
        writer = lame.createWriterFor(stream, writerOptions(outSR, nCh, 16, lameQualityIndex(opts.mp3Bitrate)));
    } else // AAC
    {
#if JUCE_WINDOWS
        juce::WindowsMediaAudioFormat wmfFmt;
        std::unique_ptr<juce::OutputStream> stream = tmpOut.getFile().createOutputStream();
        if (!stream) return "Cannot create output file: " + opts.outputPath;
        writer = wmfFmt.createWriterFor(stream, writerOptions(outSR, nCh, 16)); // AAC is encoded at 16-bit
#elif JUCE_MAC
        juce::CoreAudioFormat caFmt;
        std::unique_ptr<juce::OutputStream> stream = tmpOut.getFile().createOutputStream();
        if (!stream) return "Cannot create output file: " + opts.outputPath;
        writer = caFmt.createWriterFor(stream, writerOptions(outSR, nCh, 16));
#else
        return "AAC encoding is not supported on this platform.";
#endif
    }

    if (!writer) return "Failed to create audio writer for: " + opts.outputPath;

    constexpr int kBlockSize = 4096;
    juce::AudioBuffer<float> buf(nCh, kBlockSize);

    // Streaming resampler for rate conversion (interpolated read-through);
    // the direct reader path is kept for same-rate exports so samples pass
    // through bit-transparent (modulo gain/dither).
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    std::unique_ptr<juce::ResamplingAudioSource> resampler;
    juce::int64 total = reader->lengthInSamples;
    if (needsResample) {
        readerSource = std::make_unique<juce::AudioFormatReaderSource>(reader.get(), false);
        resampler = std::make_unique<juce::ResamplingAudioSource>(readerSource.get(), false, nCh);
        resampler->setResamplingRatio(srcRate / static_cast<double>(outSR));
        resampler->prepareToPlay(kBlockSize, outSR);
        total = static_cast<juce::int64>(std::llround(static_cast<double>(reader->lengthInSamples) * outSR / srcRate));
    }

    // 16-bit targets require TPDF dithering to avoid correlated quantisation noise.
    // 24-bit and 32-bit targets have sufficient resolution that dithering has no
    // audible effect, so we skip it to avoid unnecessary computation.
    const bool doDither = (bitD == 16);
    juce::Random ditherRng;

    for (juce::int64 pos = 0; pos < total;) {
        // Honour cancellation request between blocks
        if (opts.cancelFlag && opts.cancelFlag->load(std::memory_order_relaxed)) return "Export cancelled.";

        const int n = static_cast<int>(std::min(static_cast<juce::int64>(kBlockSize), total - pos));
        if (needsResample) {
            buf.clear();
            juce::AudioSourceChannelInfo info(&buf, 0, n);
            resampler->getNextAudioBlock(info); // zero-pads past EOF (rounding tail)
        } else {
            reader->read(&buf, 0, n, pos, true, true);
        }
        buf.applyGain(0, n, opts.gainLinear);
        if (doDither) applyTpdfDither(buf, n, nCh, ditherRng);
        if (!writer->writeFromAudioSampleBuffer(buf, 0, n))
            return "Write error at sample position " + juce::String(pos);

        if (opts.progressOut != nullptr && total > 0)
            opts.progressOut->store(static_cast<float>(pos + n) / static_cast<float>(total), std::memory_order_relaxed);
        pos += n;
    }

    if (resampler) resampler->releaseResources();

    writer.reset(); // flushes and closes the temp stream before the swap
    if (!tmpOut.overwriteTargetFileWithTemporary())
        return "Cannot replace output file: " + opts.outputPath;
    return {}; // success
}

} // namespace khensu
