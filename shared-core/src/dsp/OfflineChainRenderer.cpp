// OVH-9: the native offline chain render - see OfflineChainRenderer.h.
#include <khensu/dsp/OfflineChainRenderer.h>
#include <khensu/dsp/IirDesign.h>
#include <khensu/dsp/TruePeakOversampler.h>
#include <khensu/analysis/LoudnessAnalyzer.h>
#include <khensu/session/AudioFileProber.h>

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>

namespace khensu::dsp {

namespace {

constexpr double kGuardrailCeilingDbtp = -1.0; // render_guardrails._TP_CEILING_DBTP
constexpr double kTruePeakWorseDb = 0.1;       // _TP_WORSE_DB
constexpr double kMonoCorrelationDrop = 0.1;   // _MONO_CORR_DROP

double toDb(double linear)
{
    return 20.0 * std::log10(linear + 1e-12);
}

/// signal_integrity.analyze_mono_bass's correlation_below_cutoff, from running
/// sums: L and R through a 4th-order 120 Hz Butterworth low-pass (causal),
/// mean-removed Pearson correlation, 1 when either side is silent or the low
/// band sits 30 dB under the full mix.
class MonoBassMeter {
  public:
    void prepare(double sampleRate, int numChannels)
    {
        stereo_ = numChannels >= 2;
        lowpass_.setSections(butterworth(ButterworthType::Lowpass, 4, sampleRate, 120.0), 2);
    }
    void push(const PlanarBlock& b)
    {
        if (!stereo_ || b.ch.size() < 2) return;
        const int n = b.numSamples();
        l_.assign(b.ch[0].begin(), b.ch[0].end());
        r_.assign(b.ch[1].begin(), b.ch[1].end());
        lowpass_.process(0, l_.data(), n);
        lowpass_.process(1, r_.data(), n);
        for (int i = 0; i < n; ++i) {
            const auto k = static_cast<size_t>(i);
            const double l = l_[k], r = r_[k];
            const double full = 0.5 * (b.ch[0][k] + b.ch[1][k]);
            const double mono = 0.5 * (l + r);
            sl_ += l;
            sr_ += r;
            sll_ += l * l;
            srr_ += r * r;
            slr_ += l * r;
            sFull_ += full * full;
            sMono_ += mono * mono;
        }
        count_ += n;
    }
    double correlation() const
    {
        if (!stereo_ || count_ == 0) return 1.0;
        const auto n = static_cast<double>(count_);
        const double ml = sl_ / n, mr = sr_ / n;
        const double lStd = std::sqrt(std::max(0.0, sll_ / n - ml * ml));
        const double rStd = std::sqrt(std::max(0.0, srr_ / n - mr * mr));
        const double fullRms = std::sqrt(sFull_ / n) + 1e-12;
        const double lowRms = std::sqrt(sMono_ / n) + 1e-12;
        const bool negligible = lowRms < fullRms * std::pow(10.0, -30.0 / 20.0);
        if (lStd < 1e-9 || rStd < 1e-9 || negligible) return 1.0;
        return std::clamp((slr_ / n - ml * mr) / (lStd * rStd), -1.0, 1.0);
    }

  private:
    bool stereo_ = false;
    BiquadCascade lowpass_;
    std::vector<double> l_, r_;
    double sl_ = 0, sr_ = 0, sll_ = 0, srr_ = 0, slr_ = 0, sFull_ = 0, sMono_ = 0;
    std::int64_t count_ = 0;
};

/// Integrated loudness + 4x true peak (the shared BS.1770 meter) and the
/// low-band correlation of one stream.
class LevelMeter {
  public:
    void prepare(double sampleRate, int numChannels)
    {
        loudness_.reset(sampleRate, numChannels);
        mono_.prepare(sampleRate, numChannels);
    }
    void push(const PlanarBlock& b)
    {
        const int n = b.numSamples();
        floats_.resize(b.ch.size());
        ptrs_.resize(b.ch.size());
        for (size_t c = 0; c < b.ch.size(); ++c) {
            floats_[c].resize(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i)
                floats_[c][static_cast<size_t>(i)] = static_cast<float>(b.ch[c][static_cast<size_t>(i)]);
            ptrs_[c] = floats_[c].data();
        }
        loudness_.process(ptrs_.data(), static_cast<int>(ptrs_.size()), n);
        mono_.push(b);
    }
    RenderLevels result() const
    {
        const auto r = loudness_.getResult();
        RenderLevels out;
        if (r.lufs_integrated > -69.99f) out.integratedLufs = static_cast<double>(r.lufs_integrated);
        out.truePeakDbtp = static_cast<double>(r.true_peak_dbtp);
        out.monoLowCorrelation = mono_.correlation();
        return out;
    }

  private:
    LoudnessAnalyzer loudness_;
    MonoBassMeter mono_;
    std::vector<std::vector<float>> floats_;
    std::vector<const float*> ptrs_;
};

std::unique_ptr<juce::AudioFormatWriter> openFloatWriter(const juce::File& file, double sampleRate, int numChannels)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    if (stream == nullptr) return nullptr;
    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(sampleRate)
                             .withNumChannels(numChannels)
                             .withBitsPerSample(32)
                             .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    return wav.createWriterFor(stream, options);
}

bool writeBlock(juce::AudioFormatWriter& writer, const PlanarBlock& b, double gain, std::vector<std::vector<float>>& tmp)
{
    const int n = b.numSamples();
    tmp.resize(b.ch.size());
    std::vector<const float*> ptrs(b.ch.size());
    for (size_t c = 0; c < b.ch.size(); ++c) {
        tmp[c].resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            tmp[c][static_cast<size_t>(i)] = static_cast<float>(b.ch[c][static_cast<size_t>(i)] * gain);
        ptrs[c] = tmp[c].data();
    }
    return writer.writeFromFloatArrays(ptrs.data(), static_cast<int>(ptrs.size()), n);
}

std::string fmt(double v)
{
    std::ostringstream s;
    s.setf(std::ios::fixed);
    s.precision(2);
    s << v;
    return s.str();
}

double round2(double v)
{
    return std::round(v * 100.0) / 100.0;
}

} // namespace

bool OfflineChainRenderer::canRender(const std::vector<ChainStageSpec>& chain, int numChannels)
{
    return !chain.empty() && numChannels >= 1 && numChannels <= 2 && effectsNeedingEngine(chain).empty();
}

OfflineRenderResult OfflineChainRenderer::renderFile(const juce::File& input, const juce::File& output,
                                                     const std::vector<ChainStageSpec>& chain,
                                                     const OfflineRenderOptions& options)
{
    OfflineRenderResult res;
    res.needsEngine = effectsNeedingEngine(chain);
    if (chain.empty()) {
        res.error = "Nothing to render.";
        return res;
    }
    if (!res.needsEngine.empty()) {
        res.status = OfflineRenderResult::Status::NeedsEngine;
        std::string names;
        for (const auto& e : res.needsEngine) names += (names.empty() ? "" : ", ") + e;
        res.error = names + " needs the analysis engine.";
        return res;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    auto reader = AudioFileProber::openReader(formats, input);
    if (reader == nullptr) {
        res.error = "Could not read " + input.getFullPathName().toStdString();
        return res;
    }
    res.sampleRate = reader->sampleRate;
    res.numChannels = static_cast<int>(reader->numChannels);
    res.numSamples = reader->lengthInSamples;
    if (res.numChannels < 1 || res.numChannels > 2) {
        res.status = OfflineRenderResult::Status::NeedsEngine;
        res.error = "Surround files render with the analysis engine.";
        return res;
    }

    ChainProcessor chainProc;
    if (!chainProc.prepare(chain, res.sampleRate, res.numChannels, res.error)) return res;
    for (const auto& s : chain) res.applied.push_back(s.effect);

    // ENH-6: the key signal, when a stage keys on one.
    std::unique_ptr<juce::AudioFormatReader> keyReader;
    if (chainProc.wantsKey() && !options.keyPath.empty()) {
        keyReader = AudioFileProber::openReader(formats, juce::File(juce::String::fromUTF8(options.keyPath.c_str())));
        if (keyReader == nullptr) {
            res.warnings.push_back("Could not read the key signal; the dynamic EQ keyed on its own input.");
        } else if (std::abs(keyReader->sampleRate - res.sampleRate) > 0.5 || keyReader->numChannels < 1) {
            res.warnings.push_back("The key signal's sample rate differs from the source; the dynamic EQ keyed on "
                                   "its own input.");
            keyReader.reset();
        } else {
            chainProc.attachKey(static_cast<int>(keyReader->numChannels));
        }
    }
    const bool limiterLast = chain.back().effect == "Limiter";

    const auto cancelled = [&options] { return options.shouldCancel && options.shouldCancel(); };
    const auto progress = [&options](double p) {
        if (options.onProgress) options.onProgress(std::clamp(p, 0.0, 1.0));
    };

    const juce::File temp1 = output.getSiblingFile(output.getFileNameWithoutExtension() + ".native-render.tmp.wav");
    const juce::File temp2 = output.getSiblingFile(output.getFileNameWithoutExtension() + ".native-gain.tmp.wav");
    const auto failWith = [&](OfflineRenderResult::Status status, const std::string& error) {
        temp1.deleteFile();
        temp2.deleteFile();
        res.status = status;
        res.error = error;
        return res;
    };

    // ---- pass 0 (FN2-2a): the whole-signal profiles a stage needs first ----
    // (SpectralDenoise's noise profile). The file is decoded again for each.
    const bool profiling = chainProc.needsProfilePass();
    const double renderShare = profiling ? 0.55 : 0.85; // of the progress bar
    const double renderFrom = profiling ? 0.3 : 0.0;
    if (profiling) {
        const int blockSize = std::max(256, options.blockSize);
        bool stoppedByCancel = false, decodeFailed = false;
        const auto replay = [&](const std::function<void(PlanarBlock&&)>& consume) {
            juce::AudioBuffer<float> buffer(res.numChannels, blockSize);
            for (std::int64_t pos = 0; pos < res.numSamples; pos += blockSize) {
                if (cancelled()) {
                    stoppedByCancel = true;
                    return false;
                }
                const int n = static_cast<int>(std::min<std::int64_t>(blockSize, res.numSamples - pos));
                if (!reader->read(&buffer, 0, n, pos, true, true)) {
                    decodeFailed = true;
                    return false;
                }
                PlanarBlock block;
                block.ch.resize(static_cast<size_t>(res.numChannels));
                for (int c = 0; c < res.numChannels; ++c) {
                    const float* src = buffer.getReadPointer(c);
                    block.ch[static_cast<size_t>(c)].assign(src, src + n);
                }
                consume(std::move(block));
                progress(0.3 * static_cast<double>(pos + n) / static_cast<double>(std::max<std::int64_t>(1, res.numSamples)));
            }
            return true;
        };
        std::string profileError;
        if (!chainProc.runProfilePasses(replay, profileError)) {
            if (stoppedByCancel) return failWith(OfflineRenderResult::Status::Cancelled, {});
            return failWith(OfflineRenderResult::Status::Failed,
                            decodeFailed ? "Could not decode the source audio." : profileError);
        }
    }

    // ---- pass 1: decode -> chain -> temp, measuring both sides ----
    LevelMeter inMeter, outMeter;
    inMeter.prepare(res.sampleRate, res.numChannels);
    outMeter.prepare(res.sampleRate, res.numChannels);
    StreamingTruePeak outPeak8; // the final limiter's residual trim
    if (limiterLast) outPeak8.prepare(res.numChannels, 8);
    double outSamplePeak = 0.0;
    bool finite = true;
    std::vector<std::vector<float>> scratch;
    {
        auto writer = openFloatWriter(temp1, res.sampleRate, res.numChannels);
        if (writer == nullptr) return failWith(OfflineRenderResult::Status::Failed, "Could not write the render file.");
        bool writeOk = true;
        const auto sink = [&](const PlanarBlock& b) {
            for (const auto& c : b.ch)
                for (const double s : c) {
                    if (!std::isfinite(s)) finite = false;
                    outSamplePeak = std::max(outSamplePeak, std::abs(s));
                }
            outMeter.push(b);
            if (limiterLast) {
                std::vector<const double*> ptrs;
                for (const auto& c : b.ch) ptrs.push_back(c.data());
                outPeak8.push(ptrs.data(), static_cast<int>(ptrs.size()), b.numSamples());
            }
            writeOk = writeBlock(*writer, b, 1.0, scratch) && writeOk;
        };

        const int blockSize = std::max(256, options.blockSize);
        juce::AudioBuffer<float> buffer(res.numChannels, blockSize);
        const int keyChannels = keyReader ? static_cast<int>(keyReader->numChannels) : 0;
        juce::AudioBuffer<float> keyBuffer(std::max(1, keyChannels), keyReader ? blockSize : 1);
        PlanarBlock keyBlock;
        for (std::int64_t pos = 0; pos < res.numSamples; pos += blockSize) {
            if (cancelled()) {
                writer.reset();
                return failWith(OfflineRenderResult::Status::Cancelled, {});
            }
            const int n = static_cast<int>(std::min<std::int64_t>(blockSize, res.numSamples - pos));
            if (!reader->read(&buffer, 0, n, pos, true, true)) {
                writer.reset();
                return failWith(OfflineRenderResult::Status::Failed, "Could not decode the source audio.");
            }
            PlanarBlock block;
            block.ch.resize(static_cast<size_t>(res.numChannels));
            for (int c = 0; c < res.numChannels; ++c) {
                const float* src = buffer.getReadPointer(c);
                auto& dst = block.ch[static_cast<size_t>(c)];
                dst.resize(static_cast<size_t>(n));
                for (int i = 0; i < n; ++i) {
                    if (!std::isfinite(src[i])) {
                        writer.reset();
                        return failWith(OfflineRenderResult::Status::Failed,
                                        "Source audio contains non-finite samples");
                    }
                    dst[static_cast<size_t>(i)] = static_cast<double>(src[i]);
                }
            }
            inMeter.push(block);
            if (keyReader) {
                // Past the key's end the reader fills silence, so the key is
                // always as long as the input.
                if (!keyReader->read(&keyBuffer, 0, n, pos, true, true)) keyBuffer.clear();
                keyBlock.ch.resize(static_cast<size_t>(keyChannels));
                for (int c = 0; c < keyChannels; ++c) {
                    const float* src = keyBuffer.getReadPointer(c);
                    auto& dst = keyBlock.ch[static_cast<size_t>(c)];
                    dst.resize(static_cast<size_t>(n));
                    for (int i = 0; i < n; ++i)
                        dst[static_cast<size_t>(i)] = std::isfinite(src[i]) ? static_cast<double>(src[i]) : 0.0;
                }
                chainProc.process(std::move(block), sink, &keyBlock);
            } else {
                chainProc.process(std::move(block), sink);
            }
            progress(renderFrom + renderShare * static_cast<double>(pos + n) /
                                      static_cast<double>(std::max<std::int64_t>(1, res.numSamples)));
        }
        chainProc.finish(sink);
        if (!writer->flush() || !writeOk) {
            writer.reset();
            return failWith(OfflineRenderResult::Status::Failed, "Could not write the render file.");
        }
    }
    if (!finite) return failWith(OfflineRenderResult::Status::Failed, "DSP stage produced non-finite audio");

    // ---- the output stage, as one gain ----
    double gain = 1.0;
    if (limiterLast) {
        const double target = std::pow(10.0, limiterTargetDbtp(chain.back()) / 20.0);
        const double tp8 = outPeak8.finishLinear();
        if (tp8 > target) gain = target / tp8;
    }
    res.before = inMeter.result();
    RenderLevels after = outMeter.result();
    {
        const double ceiling = std::pow(10.0, chainOutputCeilingDbtp(chain) / 20.0);
        const double samplePeak = outSamplePeak * gain;
        if (samplePeak > 0.0 && samplePeak > ceiling * 0.7) {
            const double tp4 = std::pow(10.0, after.truePeakDbtp / 20.0) * gain;
            if (tp4 > ceiling) gain *= ceiling / tp4;
        }
    }
    res.outputGainDb = 20.0 * std::log10(gain);
    if (std::isfinite(after.integratedLufs)) after.integratedLufs += res.outputGainDb;
    after.truePeakDbtp = toDb(std::pow(10.0, after.truePeakDbtp / 20.0) * gain);
    res.after = after;

    // ---- the guardrail's absolute checks ----
    if (std::isfinite(res.before.integratedLufs) && std::isfinite(res.after.integratedLufs))
        res.matchedGainDb = round2(res.before.integratedLufs - res.after.integratedLufs);
    const double tpBefore = round2(res.before.truePeakDbtp), tpAfter = round2(res.after.truePeakDbtp);
    if (tpAfter > kGuardrailCeilingDbtp && tpAfter > tpBefore + kTruePeakWorseDb)
        res.blocks.push_back("True peak rose to " + fmt(tpAfter) + " dBTP (over the " + fmt(kGuardrailCeilingDbtp) +
                             " dBTP ceiling) - will clip after lossy encoding.");
    if (res.after.monoLowCorrelation < res.before.monoLowCorrelation - kMonoCorrelationDrop)
        res.blocks.push_back("Mono low-band correlation dropped (" + fmt(res.before.monoLowCorrelation) + " -> " +
                             fmt(res.after.monoLowCorrelation) +
                             ") - the low end will lose energy on mono/club systems.");
    res.guardrailOk = res.blocks.empty();
    if (!res.guardrailOk && options.enforceGuardrail) {
        std::string why = "Render refused by the post-render check:";
        for (const auto& b : res.blocks) why += "\n" + b;
        return failWith(OfflineRenderResult::Status::Failed, why);
    }

    // ---- pass 2: apply the output gain (or just move the file) ----
    if (std::abs(gain - 1.0) < 1e-12) {
        output.deleteFile();
        if (!temp1.moveFileTo(output))
            return failWith(OfflineRenderResult::Status::Failed, "Could not write the render file.");
    } else {
        auto tempReader = AudioFileProber::openReader(formats, temp1);
        auto writer = tempReader ? openFloatWriter(temp2, res.sampleRate, res.numChannels) : nullptr;
        if (writer == nullptr) {
            tempReader.reset();
            return failWith(OfflineRenderResult::Status::Failed, "Could not write the render file.");
        }
        const int blockSize = std::max(256, options.blockSize);
        juce::AudioBuffer<float> buffer(res.numChannels, blockSize);
        const std::int64_t total = tempReader->lengthInSamples;
        bool ok = true;
        for (std::int64_t pos = 0; pos < total && ok; pos += blockSize) {
            if (cancelled()) {
                writer.reset();
                tempReader.reset();
                return failWith(OfflineRenderResult::Status::Cancelled, {});
            }
            const int n = static_cast<int>(std::min<std::int64_t>(blockSize, total - pos));
            ok = tempReader->read(&buffer, 0, n, pos, true, true);
            buffer.applyGain(0, n, static_cast<float>(gain));
            ok = ok && writer->writeFromAudioSampleBuffer(buffer, 0, n);
            progress(0.85 + 0.15 * static_cast<double>(pos + n) / static_cast<double>(std::max<std::int64_t>(1, total)));
        }
        ok = writer->flush() && ok;
        writer.reset();
        tempReader.reset();
        if (!ok) return failWith(OfflineRenderResult::Status::Failed, "Could not write the render file.");
        output.deleteFile();
        if (!temp2.moveFileTo(output))
            return failWith(OfflineRenderResult::Status::Failed, "Could not write the render file.");
        temp1.deleteFile();
    }
    progress(1.0);
    res.status = OfflineRenderResult::Status::Rendered;
    return res;
}

} // namespace khensu::dsp
