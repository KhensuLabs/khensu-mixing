#include "MixHub.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace khensu::mixhub {

namespace {
// ~170 ms analysis window at any rate: 8192 points at 44.1/48 kHz (5.9 Hz
// bins, enough to separate a 50 Hz kick from a 70 Hz bass), 16384 at 96 kHz.
int fftOrderFor(double sampleRate) noexcept
{
    const double wanted = juce::jmax(1.0, sampleRate * 0.17);
    return juce::jlimit(12, 15, (int)std::ceil(std::log2(wanted)));
}
constexpr double kSmoothingSeconds = 0.4;
} // namespace

//==============================================================================
MixHubTap::MixHubTap() { instanceId_ = hub_->registerTap(*this); }

MixHubTap::~MixHubTap() { hub_->unregisterTap(*this); }

void MixHubTap::prepare(double sampleRate)
{
    if (!(sampleRate > 0.0)) sampleRate = 48000.0;
    // The publisher may be mid-analyse() on the old buffers: hold its lock.
    const juce::ScopedLock sl(hub_->tapsLock_);
    prepared_.store(false, std::memory_order_release);

    sampleRate_ = sampleRate;
    const int capacity = juce::jmax(4096, (int)std::ceil(sampleRate)); // one second of audio
    fifoData_.assign((size_t)capacity, 0.0f);
    fifo_.setTotalSize(capacity);
    fifo_.reset();

    const int order = fftOrderFor(sampleRate);
    const int n = 1 << order;
    fft_ = std::make_unique<juce::dsp::FFT>(order);
    history_.assign((size_t)n, 0.0f);
    window_.resize((size_t)n);
    juce::dsp::WindowingFunction<float>::fillWindowingTables(window_.data(), (size_t)n,
                                                            juce::dsp::WindowingFunction<float>::hann, false);
    fftBuffer_.assign((size_t)n * 2, 0.0f);
    binPower_.assign((size_t)n / 2, 0.0f);
    scratch_.assign((size_t)capacity, 0.0f);
    smoothedPower_.fill(0.0);
    haveSmoothed_ = false;
    lastAnalysisMs_ = 0.0;

    prepared_.store(true, std::memory_order_release);
}

void MixHubTap::pushBlock(const juce::AudioBuffer<float>& buffer, float shortTermLufs, float peakDb,
                          float width) noexcept
{
    shortTermLufs_.store(shortTermLufs, std::memory_order_relaxed);
    peakDb_.store(peakDb, std::memory_order_relaxed);
    width_.store(width, std::memory_order_relaxed);
    if (!prepared_.load(std::memory_order_acquire)) return;

    const int n = buffer.getNumSamples();
    const int channels = buffer.getNumChannels();
    if (n <= 0 || channels <= 0) return;
    const float* left = buffer.getReadPointer(0);
    const float* right = buffer.getReadPointer(channels > 1 ? 1 : 0);

    // Writes only the free space; when the publisher falls behind, the newest
    // audio is dropped rather than blocking (it reads the latest window anyway).
    const auto scope = fifo_.write(n);
    auto copy = [&](int dst, int count, int srcOffset) noexcept {
        for (int i = 0; i < count; ++i)
            fifoData_[(size_t)(dst + i)] = 0.5f * (left[srcOffset + i] + right[srcOffset + i]);
    };
    if (scope.blockSize1 > 0) copy(scope.startIndex1, scope.blockSize1, 0);
    if (scope.blockSize2 > 0) copy(scope.startIndex2, scope.blockSize2, scope.blockSize1);
}

void MixHubTap::setTrackName(const juce::String& name)
{
    const juce::ScopedLock sl(nameLock_);
    trackName_ = name;
}

juce::String MixHubTap::getTrackName() const
{
    const juce::ScopedLock sl(nameLock_);
    return trackName_;
}

void MixHubTap::setRoleOverride(const juce::String& role)
{
    const juce::ScopedLock sl(nameLock_);
    roleOverride_ = role;
}

juce::String MixHubTap::getRoleOverride() const
{
    const juce::ScopedLock sl(nameLock_);
    return roleOverride_;
}

bool MixHubTap::analyse(double nowMs, TrackFrame& out)
{
    if (!prepared_.load(std::memory_order_acquire) || fft_ == nullptr) return false;
    const int ready = fifo_.getNumReady();
    if (ready <= 0) return false;

    // Drain everything, keep the newest fftSize samples in history_.
    int got = 0;
    {
        const auto scope = fifo_.read(ready);
        if (scope.blockSize1 > 0)
            std::memcpy(scratch_.data(), fifoData_.data() + scope.startIndex1, sizeof(float) * (size_t)scope.blockSize1);
        if (scope.blockSize2 > 0)
            std::memcpy(scratch_.data() + scope.blockSize1, fifoData_.data() + scope.startIndex2,
                        sizeof(float) * (size_t)scope.blockSize2);
        got = scope.blockSize1 + scope.blockSize2;
    }
    if (got <= 0) return false;
    const int n = (int)history_.size();
    if (got >= n) {
        std::memcpy(history_.data(), scratch_.data() + (got - n), sizeof(float) * (size_t)n);
    } else {
        std::memmove(history_.data(), history_.data() + got, sizeof(float) * (size_t)(n - got));
        std::memcpy(history_.data() + (n - got), scratch_.data(), sizeof(float) * (size_t)got);
    }

    // Windowed FFT -> one-sided mean-square per bin: 2|X_k|^2 / (N * sum w^2),
    // so a full-scale sine sums to 0.5 (-3.01 dB) across its band.
    double sumW2 = 0.0;
    for (int i = 0; i < n; ++i) {
        fftBuffer_[(size_t)i] = history_[(size_t)i] * window_[(size_t)i];
        sumW2 += (double)window_[(size_t)i] * window_[(size_t)i];
    }
    std::fill(fftBuffer_.begin() + n, fftBuffer_.end(), 0.0f);
    fft_->performFrequencyOnlyForwardTransform(fftBuffer_.data(), true);
    const double norm = 2.0 / ((double)n * sumW2);
    const int bins = n / 2;
    for (int k = 0; k < bins; ++k) {
        const double mag = fftBuffer_[(size_t)k];
        binPower_[(size_t)k] = (float)(mag * mag * norm);
    }
    const auto instant = bandLevelsFromPower(binPower_.data(), bins, sampleRate_ / n);

    // Smooth in the power domain so a single transient doesn't flicker the view.
    const double dtSec = haveSmoothed_ ? juce::jmax(0.0, (nowMs - lastAnalysisMs_) / 1000.0) : 0.0;
    const double alpha = haveSmoothed_ ? 1.0 - std::exp(-dtSec / kSmoothingSeconds) : 1.0;
    BandLevels smoothed;
    for (int band = 0; band < kNumBands; ++band) {
        const float db = instant[(size_t)band];
        const double p = db <= kSilentDb ? 0.0 : std::pow(10.0, db / 10.0);
        smoothedPower_[(size_t)band] += alpha * (p - smoothedPower_[(size_t)band]);
        const double sp = smoothedPower_[(size_t)band];
        smoothed[(size_t)band] = sp > 1e-12 ? juce::jmax(kSilentDb, (float)(10.0 * std::log10(sp))) : kSilentDb;
    }
    haveSmoothed_ = true;
    lastAnalysisMs_ = nowMs;

    out.instanceId = instanceId_;
    out.trackName = getTrackName();
    out.bandDb = smoothed;
    out.userRole = getRoleOverride();
    out.role = out.userRole.isNotEmpty() ? out.userRole : guessRole(out.trackName, smoothed);
    out.shortTermLufs = shortTermLufs_.load(std::memory_order_relaxed);
    out.peakDb = peakDb_.load(std::memory_order_relaxed);
    out.width = width_.load(std::memory_order_relaxed);
    out.updatedMs = nowMs;
    return true;
}

//==============================================================================
MixHub::MixHub() : juce::Thread("Khensu Mix Hub") {}

MixHub::~MixHub() { stopThread(kStopTimeoutMs); }

juce::uint64 MixHub::registerTap(MixHubTap& tap)
{
    const auto id = nextId_.fetch_add(1);
    const juce::ScopedLock life(lifecycleLock_);
    {
        const juce::ScopedLock sl(tapsLock_);
        taps_.push_back(&tap);
    }
    // Normal, not low, priority: the work is a few FFTs per 100 ms, and this
    // thread is joined synchronously when the last instance unloads. A
    // low-priority thread can be starved for seconds on a saturated machine
    // (Windows only boosts starved threads after ~4 s), past the join timeout,
    // and JUCE then kills it by force. Host audio threads run far above normal.
    if (!isThreadRunning()) startThread(juce::Thread::Priority::normal);
    return id;
}

void MixHub::unregisterTap(MixHubTap& tap)
{
    // One lifecycle lock across the membership change and the start/stop
    // decision (PLG-17). The publisher only takes tapsLock_, never this one,
    // so stopping it while holding lifecycleLock_ cannot deadlock.
    const juce::ScopedLock life(lifecycleLock_);
    bool last = false;
    {
        // publishAll() holds this lock for its whole pass, so once we have it
        // no analyse() call can still be touching `tap`.
        const juce::ScopedLock sl(tapsLock_);
        taps_.erase(std::remove(taps_.begin(), taps_.end(), &tap), taps_.end());
        last = taps_.empty();
    }
    {
        const juce::ScopedLock sl(framesLock_);
        frames_.erase(tap.getInstanceId());
    }
    if (last) stopThread(kStopTimeoutMs);
}

void MixHub::publishAll(double nowMs)
{
    const juce::ScopedLock sl(tapsLock_);
    for (auto* tap : taps_) {
        TrackFrame frame;
        if (tap->analyse(nowMs, frame)) publish(frame);
    }
}

void MixHub::publish(const TrackFrame& frame)
{
    const juce::ScopedLock sl(framesLock_);
    frames_[frame.instanceId] = frame;
}

std::vector<TrackFrame> MixHub::snapshot(double nowMs) const
{
    const double expiry = expiryMs_.load();
    std::vector<TrackFrame> fresh;
    const juce::ScopedLock sl(framesLock_);
    for (const auto& [id, frame] : frames_)
        if (nowMs - frame.updatedMs <= expiry) fresh.push_back(frame);
    return fresh;
}

int MixHub::getNumRegistered() const
{
    const juce::ScopedLock sl(tapsLock_);
    return (int)taps_.size();
}

void MixHub::run()
{
    while (!threadShouldExit()) {
        if (autoPublish_.load()) publishAll(nowMs());
        wait(kPublishIntervalMs);
    }
}

} // namespace khensu::mixhub
