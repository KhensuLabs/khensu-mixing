// desktop-app/Source/ui/shared/TransportWaveform.h
// UX2-5: the transport's scrubber - a compact waveform of the file being
// auditioned, with the playhead, click / drag to seek, and the regions the
// current fix changed (NEW-4's /analysis/changed_regions between the version
// the fix was rendered from and the fix) shaded. A changed region's marker
// (the strip under the waveform), or a double-click inside it, loops it.
//
// Peaks are read off the message thread (IBackgroundWork, session-epoch
// checked) and kept per file (path + size + modification time) in a small
// LRU, so an A/B switch between two files does not decode either again.
// Keyboard: Left / Right seek 5 s (Shift: 1 s), Home / End, and L loops the
// region under the playhead. Theme-aware: everything is read from KhensuTheme
// in paint().
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace khensu {

class IAppController;

namespace transport_waveform {

using Regions = std::vector<std::pair<double, double>>;

/// Absolute peaks of `reader`, `numPeaks` buckets over the whole file (all
/// channels folded). `shouldStop` is polled between buckets; an abandoned
/// scan returns an empty vector.
inline std::vector<float> computePeaks(juce::AudioFormatReader& reader, int numPeaks,
                                       const std::function<bool()>& shouldStop = {})
{
    std::vector<float> peaks;
    const juce::int64 total = reader.lengthInSamples;
    if (numPeaks <= 0 || total <= 0 || reader.numChannels == 0) return peaks;
    peaks.assign(static_cast<size_t>(numPeaks), 0.0f);
    const int channels = static_cast<int>(reader.numChannels);
    juce::AudioBuffer<float> buffer(channels, 4096);
    for (int i = 0; i < numPeaks; ++i) {
        if (shouldStop && (i & 15) == 0 && shouldStop()) return {};
        const juce::int64 start = total * i / numPeaks;
        const juce::int64 end = juce::jmax(start + 1, total * (i + 1) / numPeaks);
        float maxVal = 0.0f;
        for (juce::int64 pos = start; pos < end;) {
            const int n = static_cast<int>(juce::jmin<juce::int64>(4096, end - pos));
            reader.read(&buffer, 0, n, pos, true, true);
            for (int ch = 0; ch < channels; ++ch)
                maxVal = juce::jmax(maxVal, buffer.getMagnitude(ch, 0, n));
            pos += n;
        }
        peaks[static_cast<size_t>(i)] = juce::jlimit(0.0f, 1.0f, maxVal);
    }
    return peaks;
}

/// The cache key of a file: its path plus size and modification time, so a
/// file rewritten under the same name is read again.
inline juce::String cacheKey(const juce::File& file)
{
    return file.getFullPathName() + "|" + juce::String(file.getSize()) + "|" +
           juce::String(file.getLastModificationTime().toMilliseconds());
}

/// Least-recently-used peak cache (message thread only).
class PeakCache {
  public:
    explicit PeakCache(size_t capacity = 8) : capacity_(juce::jmax<size_t>(1, capacity)) {}

    std::shared_ptr<const std::vector<float>> find(const juce::String& key)
    {
        auto it = map_.find(key);
        if (it == map_.end()) return nullptr;
        order_.splice(order_.begin(), order_, it->second.second); // most recent first
        return it->second.first;
    }

    void put(const juce::String& key, std::vector<float> peaks)
    {
        auto value = std::make_shared<const std::vector<float>>(std::move(peaks));
        if (auto it = map_.find(key); it != map_.end()) {
            it->second.first = value;
            order_.splice(order_.begin(), order_, it->second.second);
            return;
        }
        order_.push_front(key);
        map_[key] = {value, order_.begin()};
        while (map_.size() > capacity_) {
            map_.erase(order_.back());
            order_.pop_back();
        }
    }

    size_t size() const noexcept { return map_.size(); }

  private:
    size_t capacity_;
    std::list<juce::String> order_;
    std::map<juce::String, std::pair<std::shared_ptr<const std::vector<float>>, std::list<juce::String>::iterator>> map_;
};

/// Seconds at x within a waveform `width` pixels wide showing `duration` s.
inline double timeAtX(float x, float width, double duration) noexcept
{
    if (width <= 0.0f || duration <= 0.0) return 0.0;
    return juce::jlimit(0.0, duration, static_cast<double>(x / width) * duration);
}

inline float xForTime(double seconds, float width, double duration) noexcept
{
    if (width <= 0.0f || duration <= 0.0) return 0.0f;
    return static_cast<float>(juce::jlimit(0.0, 1.0, seconds / duration)) * width;
}

/// Index of the region containing `seconds`, or -1.
inline int regionAt(const Regions& regions, double seconds) noexcept
{
    for (size_t i = 0; i < regions.size(); ++i)
        if (seconds >= regions[i].first && seconds < regions[i].second) return static_cast<int>(i);
    return -1;
}

/// The keyboard seek target: Left / Right step by `step` s, Home / End jump.
inline double keyboardSeek(double position, double duration, int keyCode, double step) noexcept
{
    if (keyCode == juce::KeyPress::leftKey) return juce::jmax(0.0, position - step);
    if (keyCode == juce::KeyPress::rightKey) return juce::jmin(duration, position + step);
    if (keyCode == juce::KeyPress::homeKey) return 0.0;
    if (keyCode == juce::KeyPress::endKey) return juce::jmax(0.0, duration - 0.05);
    return position;
}

/// One value per column: the loudest bucket that falls in it.
inline std::vector<float> columns(const std::vector<float>& peaks, int width)
{
    std::vector<float> out;
    if (width <= 0 || peaks.empty()) return out;
    out.assign(static_cast<size_t>(width), 0.0f);
    const size_t n = peaks.size();
    for (int x = 0; x < width; ++x) {
        const size_t a = n * static_cast<size_t>(x) / static_cast<size_t>(width);
        const size_t b = juce::jmax(a + 1, n * static_cast<size_t>(x + 1) / static_cast<size_t>(width));
        float m = 0.0f;
        for (size_t i = a; i < b && i < n; ++i)
            m = juce::jmax(m, peaks[i]);
        out[static_cast<size_t>(x)] = m;
    }
    return out;
}

} // namespace transport_waveform

class TransportWaveform final : public juce::Component, public juce::SettableTooltipClient {
  public:
    static constexpr int kPeakCount = 1200;

    /// Built in AppController's initializer list (inside the transport bar):
    /// the constructor never calls the controller.
    explicit TransportWaveform(IAppController& controller);
    ~TransportWaveform() override;

    /// Polled by the transport bar's timer: the audition file, the playhead
    /// and the current fix's changed regions.
    void update();

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void focusGained(FocusChangeType) override { repaint(); }
    void focusLost(FocusChangeType) override { repaint(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    /// Tests / a11y: what the scrubber shows.
    const transport_waveform::Regions& changedRegions() const noexcept { return regions_; }
    bool hasPeaks() const noexcept { return peaks_ != nullptr && !peaks_->empty(); }
    double position() const noexcept { return position_; }
    double duration() const noexcept { return duration_; }
    void seekTo(double seconds);

  private:
    void requestPeaks(const juce::File& file, const juce::String& key);
    void requestRegions(const juce::String& before, const juce::String& after);
    void loopRegion(int index);
    juce::Rectangle<float> waveArea() const;
    juce::Rectangle<float> markerArea() const;
    void updateTooltip();

    IAppController& controller_;
    transport_waveform::PeakCache cache_;
    std::set<juce::String> pendingPeaks_;
    std::shared_ptr<const std::vector<float>> peaks_;
    std::vector<float> columns_;
    juce::String auditionKey_;
    juce::String auditionPath_;

    transport_waveform::Regions regions_;
    juce::String regionsFor_;  ///< the fix the regions belong to
    juce::String regionsAsked_; ///< the fix a request is out (or done) for
    juce::uint32 regionsRetryAt_ = 0;

    double position_ = 0.0;
    double duration_ = 0.0;
    bool dragging_ = false;
    double dragSeconds_ = 0.0;
    int hoverRegion_ = -1;
    float hoverX_ = -1.0f;
    float lastPlayheadX_ = -1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportWaveform)
};

} // namespace khensu
