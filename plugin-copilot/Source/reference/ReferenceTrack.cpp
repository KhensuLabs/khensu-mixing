#include "ReferenceTrack.h"

#include <cmath>

namespace khensu::copilot::reference {

namespace {
// The hub's analysis size (MixHub.cpp fftOrderFor): ~170 ms at any rate.
int hubFftOrder(double sampleRate) noexcept
{
    const double wanted = juce::jmax(1.0, sampleRate * 0.17);
    return juce::jlimit(12, 15, static_cast<int>(std::ceil(std::log2(wanted))));
}
constexpr int kReadChunk = 1 << 16;
} // namespace

bool isReferenceFile(const juce::File& file)
{
    const auto ext = file.getFileExtension().toLowerCase();
    for (const auto* e : {".wav", ".aif", ".aiff", ".flac", ".mp3", ".ogg", ".m4a"})
        if (ext == e) return true;
    return false;
}

float ReferenceData::shortTermAt(juce::int64 position) const noexcept
{
    if (shortTermLufs.empty() || blockSamples <= 0 || position < 0) return -96.0f;
    const auto index = static_cast<size_t>(position / blockSamples);
    return shortTermLufs[std::min(index, shortTermLufs.size() - 1)];
}

mixhub::BandLevels averageBands(const juce::AudioBuffer<float>& audio, double sampleRate)
{
    const int total = audio.getNumSamples();
    const int channels = juce::jmin(2, audio.getNumChannels());
    if (total <= 0 || channels <= 0) return mixhub::silentBands();
    const int order = hubFftOrder(sampleRate);
    const int n = 1 << order;
    juce::dsp::FFT fft(order);
    std::vector<float> window(static_cast<size_t>(n));
    juce::dsp::WindowingFunction<float>::fillWindowingTables(window.data(), static_cast<size_t>(n),
                                                             juce::dsp::WindowingFunction<float>::hann, false);
    double sumW2 = 0.0;
    for (const float w : window)
        sumW2 += static_cast<double>(w) * w;
    std::vector<float> buffer(static_cast<size_t>(n) * 2);
    std::vector<double> power(static_cast<size_t>(n / 2), 0.0);
    int frames = 0;
    const int hop = n / 2;
    for (int start = 0; start == 0 || start + n <= total; start += hop) {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        const int len = juce::jmin(n, total - start);
        for (int i = 0; i < len; ++i) {
            float mono = 0.0f;
            for (int c = 0; c < channels; ++c)
                mono += audio.getSample(c, start + i);
            buffer[static_cast<size_t>(i)] = mono / static_cast<float>(channels) * window[static_cast<size_t>(i)];
        }
        fft.performFrequencyOnlyForwardTransform(buffer.data(), true);
        for (int k = 0; k < n / 2; ++k) {
            const double mag = buffer[static_cast<size_t>(k)];
            power[static_cast<size_t>(k)] += mag * mag;
        }
        ++frames;
        if (start + n >= total) break;
    }
    const double norm = 2.0 / (static_cast<double>(n) * sumW2 * juce::jmax(1, frames));
    std::vector<float> binPower(power.size());
    for (size_t k = 0; k < power.size(); ++k)
        binPower[k] = static_cast<float>(power[k] * norm);
    return mixhub::bandLevelsFromPower(binPower.data(), static_cast<int>(binPower.size()), sampleRate / n);
}

mixhub::BandLevels normalisedShape(const mixhub::BandLevels& bands)
{
    double total = 0.0;
    for (const float db : bands)
        if (db > mixhub::kSilentDb) total += std::pow(10.0, db / 10.0);
    mixhub::BandLevels out = mixhub::silentBands();
    if (total <= 1.0e-12) return out;
    const double totalDb = 10.0 * std::log10(total);
    for (size_t b = 0; b < bands.size(); ++b)
        out[b] = bands[b] > mixhub::kSilentDb ? static_cast<float>(bands[b] - totalDb) : mixhub::kSilentDb;
    return out;
}

namespace {
std::shared_ptr<ReferenceData> analyse(juce::AudioBuffer<float> audio, double sampleRate, const juce::String& path)
{
    auto data = std::make_shared<ReferenceData>();
    data->path = path;
    data->sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    data->audio = std::move(audio);
    data->blockSamples = juce::jmax(1, static_cast<int>(std::lround(data->sampleRate * 0.1)));

    LoudnessStats stats;
    stats.prepare(data->sampleRate);
    const int total = data->audio.getNumSamples();
    const int channels = juce::jmin(2, data->audio.getNumChannels());
    data->shortTermLufs.reserve(static_cast<size_t>(total / data->blockSamples + 1));
    for (int start = 0; start < total; start += data->blockSamples) {
        const int len = juce::jmin(data->blockSamples, total - start);
        const float* ptrs[2] = {data->audio.getReadPointer(0, start),
                                data->audio.getReadPointer(channels > 1 ? 1 : 0, start)};
        stats.process(ptrs, channels, len);
        data->shortTermLufs.push_back(stats.shortTermLufs());
    }
    data->stats = stats.result();
    data->bands = averageBands(data->audio, data->sampleRate);
    return data;
}
} // namespace

ReferencePtr analyseReference(juce::AudioBuffer<float> audio, double sampleRate, const juce::String& path)
{
    return analyse(std::move(audio), sampleRate, path);
}

ReferencePtr loadReference(const juce::File& file, double sampleRate, juce::String& error, const CancelCheck& cancelled)
{
    const auto isCancelled = [&cancelled] { return cancelled && cancelled(); };
    if (!file.existsAsFile()) {
        error = TRANS("The reference file does not exist.");
        return nullptr;
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->numChannels == 0) {
        error = TRANS("The reference file could not be decoded.");
        return nullptr;
    }
    const double fileRate = reader->sampleRate;
    const auto maxSamples = static_cast<juce::int64>(kMaxReferenceSeconds * fileRate);
    const auto length = juce::jmin(reader->lengthInSamples, maxSamples);
    if (length <= 0) {
        error = TRANS("The reference file holds no audio.");
        return nullptr;
    }
    const int channels = juce::jmin(2, static_cast<int>(reader->numChannels));
    juce::AudioBuffer<float> decoded(channels, static_cast<int>(length));
    for (juce::int64 start = 0; start < length; start += kReadChunk) {
        if (isCancelled()) return nullptr;
        const int len = static_cast<int>(juce::jmin<juce::int64>(kReadChunk, length - start));
        reader->read(&decoded, static_cast<int>(start), len, start, true, channels > 1);
    }

    const double target = sampleRate > 0.0 ? sampleRate : fileRate;
    juce::AudioBuffer<float> audio;
    if (std::abs(fileRate - target) < 0.5) {
        audio = std::move(decoded);
    } else {
        const double ratio = fileRate / target;
        const int outLength = static_cast<int>(std::floor(static_cast<double>(length) / ratio));
        audio.setSize(channels, juce::jmax(1, outLength));
        audio.clear();
        for (int c = 0; c < channels; ++c) {
            if (isCancelled()) return nullptr;
            juce::WindowedSincInterpolator interpolator;
            interpolator.process(ratio, decoded.getReadPointer(c), audio.getWritePointer(c), outLength,
                                 decoded.getNumSamples(), 0);
        }
    }
    if (isCancelled()) return nullptr;
    auto data = analyse(std::move(audio), target, file.getFullPathName());
    data->truncated = reader->lengthInSamples > maxSamples;
    return data;
}

//==============================================================================
ReferencePlayer::~ReferencePlayer()
{
    delete pending_.exchange(nullptr);
    delete retired_.exchange(nullptr);
    delete active_;
}

void ReferencePlayer::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    fadeSamples_ = juce::jmax(1, static_cast<int>(std::lround(sampleRate_ * kFadeSeconds)));
    mix_ = 0.0f;
    haveGain_ = false;
    mixOut_.store(0.0f, std::memory_order_relaxed);
}

void ReferencePlayer::setReference(ReferencePtr data)
{
    const juce::SpinLock::ScopedLockType sl(controlLock_);
    delete retired_.exchange(nullptr, std::memory_order_acq_rel);
    auto* holder = new Holder{std::move(data)};
    // A reference set twice before the audio thread ran keeps only the latest.
    delete pending_.exchange(holder, std::memory_order_acq_rel);
}

void ReferencePlayer::reapRetired()
{
    const juce::SpinLock::ScopedLockType sl(controlLock_);
    delete retired_.exchange(nullptr, std::memory_order_acq_rel);
}

void ReferencePlayer::adoptPending() noexcept
{
    if (retired_.load(std::memory_order_acquire) != nullptr) return; // previous swap not reaped yet
    if (auto* next = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
        retired_.store(active_, std::memory_order_release);
        active_ = next;
        position_ = 0;
        haveGain_ = false;
    }
}

float ReferencePlayer::targetOffsetDb(const ReferenceData& ref, const HostSide& host, bool& ok) const noexcept
{
    ok = false;
    const auto valid = [](float lufs) { return lufs > -90.0f && std::isfinite(lufs); };
    float hostLufs = -96.0f, refLufs = -96.0f;
    if (static_cast<MatchMode>(matchMode_.load(std::memory_order_relaxed)) == MatchMode::ShortTerm) {
        hostLufs = host.shortTermLufs;
        refLufs = ref.shortTermAt(position_);
    } else {
        // Integrated on both sides; until the host's integrated value exists
        // its short-term loudness stands in.
        hostLufs = valid(host.integratedLufs) ? host.integratedLufs : host.shortTermLufs;
        refLufs = ref.stats.integratedLufs;
    }
    if (!valid(hostLufs) || !valid(refLufs)) return gainDb_;
    ok = true;
    return juce::jlimit(-kMaxOffsetDb, kMaxOffsetDb, hostLufs - refLufs);
}

void ReferencePlayer::process(juce::AudioBuffer<float>& buffer, const HostSide& host) noexcept
{
    adoptPending();
    const int n = buffer.getNumSamples();
    const int channels = juce::jmin(2, buffer.getNumChannels());
    if (n <= 0 || channels <= 0) return;

    const ReferenceData* ref = active_ != nullptr ? active_->data.get() : nullptr;
    const bool playable = ref != nullptr && ref->audio.getNumSamples() > 0 && ref->audio.getNumChannels() > 0 &&
                          std::abs(ref->sampleRate - sampleRate_) < 0.5;
    playable_.store(playable, std::memory_order_relaxed);
    const juce::int64 length = playable ? ref->audio.getNumSamples() : 0;

    // Where the reference plays: the host timeline (wrapped) when it has one,
    // else a free-running position while audio flows.
    if (playable && host.advance) {
        if (host.timeValid) {
            const auto at = static_cast<juce::int64>(std::llround(juce::jmax(0.0, host.timeSec) * sampleRate_));
            position_ = at % length;
        }
    }

    const bool wantRef = playable && !host.offline && listening_.load(std::memory_order_relaxed);
    const float mixTarget = wantRef ? 1.0f : 0.0f;
    if (!wantRef && mix_ <= 0.0f) {
        // Nothing to hear: keep the free-running position moving and publish.
        if (playable && host.advance && !host.timeValid) position_ = (position_ + n) % length;
        mixOut_.store(0.0f, std::memory_order_relaxed);
        if (playable) positionSec_.store(static_cast<double>(position_) / sampleRate_, std::memory_order_relaxed);
        return;
    }

    bool ok = false;
    const float target = playable ? targetOffsetDb(*ref, host, ok) : gainDb_;
    if (ok) matched_.store(true, std::memory_order_relaxed);
    float startDb = gainDb_;
    if (!haveGain_ && ok) {
        // The first match is taken at once, not slid into.
        gainDb_ = startDb = target;
        haveGain_ = true;
    } else {
        const double alpha = 1.0 - std::exp(-static_cast<double>(n) / (kGainSmoothingSeconds * sampleRate_));
        gainDb_ += static_cast<float>(alpha) * (target - gainDb_);
    }
    const float g0 = juce::Decibels::decibelsToGain(startDb);
    const float g1 = juce::Decibels::decibelsToGain(gainDb_);
    offsetDb_.store(gainDb_, std::memory_order_relaxed);

    const float step = 1.0f / static_cast<float>(fadeSamples_);
    const int refChannels = playable ? ref->audio.getNumChannels() : 0;
    juce::int64 pos = position_;
    for (int i = 0; i < n; ++i) {
        if (mix_ < mixTarget)
            mix_ = juce::jmin(mixTarget, mix_ + step);
        else if (mix_ > mixTarget)
            mix_ = juce::jmax(mixTarget, mix_ - step);
        const float g = g0 + (g1 - g0) * static_cast<float>(i + 1) / static_cast<float>(n);
        for (int c = 0; c < channels; ++c) {
            float r = 0.0f;
            if (playable && host.advance)
                r = ref->audio.getSample(juce::jmin(c, refChannels - 1), static_cast<int>(pos)) * g;
            float* d = buffer.getWritePointer(c);
            d[i] = d[i] + mix_ * (r - d[i]);
        }
        if (playable && host.advance) pos = (pos + 1) % length;
    }
    if (playable && host.advance) position_ = pos;
    mixOut_.store(mix_, std::memory_order_relaxed);
    if (playable) positionSec_.store(static_cast<double>(position_) / sampleRate_, std::memory_order_relaxed);
}

} // namespace khensu::copilot::reference
