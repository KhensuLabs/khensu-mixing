// desktop-app/Source/session/PlaybackMeterTap.h
// UI-5: the desktop meter bridge's audio half and its message-thread half.
//
// PlaybackMeterTap sits at the end of PlaybackEngine's chain (what the device
// plays) and, on the audio thread, K-weights each channel (the BS.1770 shelf
// + RLB high-pass of analysis/NativeMeters), accumulates 100 ms buckets of
// mean square, a 4x interpolated (true) peak per channel and the L/R
// products for correlation, and hands each finished bucket over through a
// lock-free FIFO. The raw samples go to a StereoSampleRing for the
// vectorscope. Nothing on the audio thread allocates, locks or waits: the
// FIFO and the ring are allocated up front; a full FIFO drops buckets.
//
// LiveLoudness (message thread) turns buckets into momentary (400 ms),
// short-term (3 s) and gated integrated loudness, peaks and correlation.
// THREAD_CONTEXT: process()/prepare() -> AudioThread; the rest -> UIThread.
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/NativeMeters.h>
#include <khensu/ui/StereoSampleRing.h>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace khensu {

class PlaybackMeterTap {
  public:
    struct Bucket {
        std::array<double, 2> kSumSq{}; ///< K-weighted sum of squares, per channel
        std::array<float, 2> peak{};    ///< linear true peak (4x interpolated), per channel
        double sumLR = 0.0, sumLL = 0.0, sumRR = 0.0;
        int samples = 0;
        int channels = 2;
    };

    PlaybackMeterTap() = default;

    /// Audio thread (prepareToPlay): filter coefficients for `sampleRate`.
    void prepare(double sampleRate) noexcept
    {
        const auto k = native::kWeighting(sampleRate > 0.0 ? sampleRate : 48000.0);
        for (int i = 0; i < 5; ++i) {
            shelf_[(size_t)i] = k.shelf[i];
            rlb_[(size_t)i] = k.rlb[i];
        }
        hop_ = juce::jmax(1, (int)std::lround((sampleRate > 0.0 ? sampleRate : 48000.0) * 0.1));
        for (auto& c : state_)
            c = {};
        current_ = {};
    }

    /// Audio thread: measure what is about to be played.
    void process(const juce::AudioSourceChannelInfo& info) noexcept
    {
        if (info.buffer == nullptr || info.numSamples <= 0) return;
        const int channels = juce::jmin(2, info.buffer->getNumChannels());
        if (channels <= 0) return;
        const float* l = info.buffer->getReadPointer(0, info.startSample);
        const float* r = channels > 1 ? info.buffer->getReadPointer(1, info.startSample) : l;
        scope_.push(l, r, info.numSamples);
        scopeLarge_.push(l, r, info.numSamples); // the Mix / Master scope (its own reader)
        width_.push(l, r, info.numSamples); // VIZ-4b: per-band width (its own consumer)
        spectrum_.push(l, r, info.numSamples); // VIZ-4a: the live spectrogram (its own consumer)

        for (int i = 0; i < info.numSamples; ++i) {
            const float x[2] = {l[i], r[i]};
            for (int ch = 0; ch < 2; ++ch) {
                auto& st = state_[(size_t)ch];
                const double y = filter(rlb_, st.rlb, filter(shelf_, st.shelf, (double)x[ch]));
                current_.kSumSq[(size_t)ch] += y * y;
                current_.peak[(size_t)ch] = juce::jmax(current_.peak[(size_t)ch], interpolatedPeak(st, x[ch]));
            }
            current_.sumLR += (double)x[0] * x[1];
            current_.sumLL += (double)x[0] * x[0];
            current_.sumRR += (double)x[1] * x[1];
            if (++current_.samples >= hop_) {
                current_.channels = channels;
                int start1, size1, start2, size2;
                fifo_.prepareToWrite(1, start1, size1, start2, size2);
                if (size1 > 0) buckets_[(size_t)start1] = current_;
                fifo_.finishedWrite(size1 + size2);
                current_ = {};
            }
        }
    }

    /// Message thread: takes up to `max` finished buckets, oldest first.
    int pop(Bucket* out, int max) noexcept
    {
        int start1, size1, start2, size2;
        fifo_.prepareToRead(max, start1, size1, start2, size2);
        for (int i = 0; i < size1; ++i)
            out[i] = buckets_[(size_t)(start1 + i)];
        for (int i = 0; i < size2; ++i)
            out[size1 + i] = buckets_[(size_t)(start2 + i)];
        fifo_.finishedRead(size1 + size2);
        return size1 + size2;
    }

    /// The vectorscope's sample source (consumer: the scope).
    ui::StereoSampleRing& scopeRing() noexcept { return scope_; }
    /// The large (Mix / Master) vectorscope's copy: a ring has one reader,
    /// and the transport's scope drained the shared one, leaving it empty.
    ui::StereoSampleRing& scopeLargeRing() noexcept { return scopeLarge_; }
    /// VIZ-4b: the same samples for the per-band stereo width (one reader).
    ui::StereoSampleRing& widthRing() noexcept { return width_; }
    /// VIZ-4a: the same samples for the live scrolling spectrogram (one reader).
    ui::StereoSampleRing& spectrumRing() noexcept { return spectrum_; }

    /// 4x Hermite interpolation between the last two samples - an
    /// inter-sample peak estimate cheap enough for every sample.
    struct PeakState {
        float h[3] = {0.0f, 0.0f, 0.0f};
    };
    static float interpolatedPeakOf(PeakState& st, float x) noexcept
    {
        const float y0 = st.h[0], y1 = st.h[1], y2 = st.h[2], y3 = x;
        float m = std::abs(y2);
        for (float t : {0.25f, 0.5f, 0.75f}) {
            const float c0 = y1, c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3,
                        c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            m = juce::jmax(m, std::abs(((c3 * t + c2) * t + c1) * t + c0));
        }
        st.h[0] = y1;
        st.h[1] = y2;
        st.h[2] = y3;
        return juce::jmax(m, std::abs(x));
    }

  private:
    struct Biquad {
        double z1 = 0.0, z2 = 0.0;
    };
    struct ChannelState {
        Biquad shelf, rlb;
        PeakState peak;
    };
    static double filter(const std::array<double, 5>& c, Biquad& s, double x) noexcept
    {
        // {b0, b1, b2, a1, a2}, transposed direct form II.
        const double y = c[0] * x + s.z1;
        s.z1 = c[1] * x - c[3] * y + s.z2;
        s.z2 = c[2] * x - c[4] * y;
        return y;
    }
    static float interpolatedPeak(ChannelState& st, float x) noexcept { return interpolatedPeakOf(st.peak, x); }

    static constexpr int kFifoSize = 256;
    std::array<double, 5> shelf_{1.0, 0.0, 0.0, 0.0, 0.0}, rlb_{1.0, 0.0, 0.0, 0.0, 0.0};
    std::array<ChannelState, 2> state_{};
    Bucket current_;
    int hop_ = 4800;
    juce::AbstractFifo fifo_{kFifoSize};
    std::array<Bucket, kFifoSize> buckets_{};
    ui::StereoSampleRing scope_{8192};
    ui::StereoSampleRing scopeLarge_{8192};
    ui::StereoSampleRing width_{16384};
    ui::StereoSampleRing spectrum_{16384};
};

/// Message thread: momentary / short-term / integrated loudness, peaks and
/// correlation from the tap's 100 ms buckets.
class LiveLoudness {
  public:
    static constexpr float kSilence = -100.0f;

    void add(const PlaybackMeterTap::Bucket& b)
    {
        recent_.push_back(b);
        if (recent_.size() > 30) recent_.erase(recent_.begin()); // 3 s
        peak_ = {juce::jmax(peak_[0], b.peak[0]), juce::jmax(peak_[1], b.peak[1])};
        const double m = blockPower(4);
        if (recent_.size() >= 4 && m > 0.0) blocks_.push_back(m); // one 400 ms block per 100 ms hop
    }

    void reset()
    {
        recent_.clear();
        blocks_.clear();
        resetPeaks();
    }
    void resetPeaks() { peak_ = {0.0f, 0.0f}; }

    float momentary() const { return lufsOf(blockPower(4)); }
    float shortTerm() const { return lufsOf(blockPower(30)); }

    /// BS.1770 gating: -70 LUFS absolute, then -10 LU below the absolute-gated mean.
    float integrated() const
    {
        double sum = 0.0;
        int n = 0;
        const double absGate = std::pow(10.0, (-70.0 + 0.691) / 10.0);
        for (double p : blocks_)
            if (p > absGate) {
                sum += p;
                ++n;
            }
        if (n == 0) return kSilence;
        const double relGate = (sum / n) * std::pow(10.0, -10.0 / 10.0);
        sum = 0.0;
        n = 0;
        for (double p : blocks_)
            if (p > absGate && p > relGate) {
                sum += p;
                ++n;
            }
        return n == 0 ? kSilence : lufsOf(sum / n);
    }

    /// Peaks since the last resetPeaks(), dBTP.
    float truePeakDb(int ch) const { return dbOf(peak_[(size_t)juce::jlimit(0, 1, ch)]); }

    /// Pearson correlation over the last 300 ms (1 for silence / mono).
    float correlation() const
    {
        double lr = 0.0, ll = 0.0, rr = 0.0;
        const size_t from = recent_.size() > 3 ? recent_.size() - 3 : 0;
        for (size_t i = from; i < recent_.size(); ++i) {
            lr += recent_[i].sumLR;
            ll += recent_[i].sumLL;
            rr += recent_[i].sumRR;
        }
        const double d = std::sqrt(ll * rr);
        return d > 1.0e-12 ? (float)juce::jlimit(-1.0, 1.0, lr / d) : 1.0f;
    }

    static float lufsOf(double meanSquareSum)
    {
        return meanSquareSum > 1.0e-12 ? (float)(-0.691 + 10.0 * std::log10(meanSquareSum)) : kSilence;
    }
    static float dbOf(float linear) { return linear > 1.0e-6f ? 20.0f * std::log10(linear) : kSilence; }

  private:
    /// Mean square summed over the channels (weight 1 for L and R) across the
    /// last `buckets` buckets.
    double blockPower(size_t buckets) const
    {
        if (recent_.empty()) return 0.0;
        const size_t from = recent_.size() > buckets ? recent_.size() - buckets : 0;
        double energy = 0.0;
        long long samples = 0;
        for (size_t i = from; i < recent_.size(); ++i) {
            const auto& b = recent_[i];
            energy += b.kSumSq[0] + (b.channels > 1 ? b.kSumSq[1] : b.kSumSq[0]);
            samples += b.samples;
        }
        return samples > 0 ? energy / (double)samples : 0.0;
    }

    std::vector<PlaybackMeterTap::Bucket> recent_;
    std::vector<double> blocks_;
    std::array<float, 2> peak_{0.0f, 0.0f};
};

} // namespace khensu
