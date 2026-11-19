// desktop-app/Source/ui/shared/EditViewState.h
// Pure state behind the waveform/spectrogram editing view: zoomed time
// window, selection, playhead scrubbing, and time-ranged text annotations
// with JSON persistence. Unit-tested in tests/ui/EditViewStateTest.cpp.
#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <vector>

namespace khensu {

class EditViewState {
  public:
    struct Annotation {
        int id = 0;
        double startSec = 0.0, endSec = 0.0; ///< start == end for a point marker
        juce::String text;
    };

    static constexpr double kMinVisibleSec = 0.05;

    void setDuration(double seconds)
    {
        duration_ = std::max(0.0, seconds);
        viewStart_ = 0.0;
        viewLength_ = duration_;
        clearSelection();
        playhead_ = 0.0;
    }
    double duration() const noexcept { return duration_; }

    // ---- view window -------------------------------------------------------
    double viewStart() const noexcept { return viewStart_; }
    double viewLength() const noexcept { return viewLength_; }
    double viewEnd() const noexcept { return viewStart_ + viewLength_; }

    /// Zooms by `factor` (>1 zooms in) keeping `anchorSec` under the cursor.
    void zoomAround(double anchorSec, double factor)
    {
        if (duration_ <= 0.0 || factor <= 0.0) return;
        const double newLen = std::clamp(viewLength_ / factor, std::min(kMinVisibleSec, duration_), duration_);
        const double frac = viewLength_ > 0.0 ? (anchorSec - viewStart_) / viewLength_ : 0.0;
        viewLength_ = newLen;
        setViewStart(anchorSec - frac * newLen);
    }

    /// Zooms so [start, end] fills the view (brush-to-zoom).
    void zoomTo(double start, double end)
    {
        if (end < start) std::swap(start, end);
        viewLength_ = std::clamp(end - start, std::min(kMinVisibleSec, duration_), duration_);
        setViewStart(start);
    }

    void zoomToFit()
    {
        viewStart_ = 0.0;
        viewLength_ = duration_;
    }

    void scrollBy(double seconds) { setViewStart(viewStart_ + seconds); }

    void setViewStart(double s) { viewStart_ = std::clamp(s, 0.0, std::max(0.0, duration_ - viewLength_)); }

    /// Pixel <-> time for a view `width` pixels wide starting at x = 0.
    double timeForX(float x, float width) const
    {
        if (width <= 0.0f) return viewStart_;
        return std::clamp(viewStart_ + static_cast<double>(x / width) * viewLength_, 0.0, duration_);
    }
    float xForTime(double t, float width) const
    {
        if (viewLength_ <= 0.0) return 0.0f;
        return static_cast<float>((t - viewStart_) / viewLength_) * width;
    }

    // ---- playhead / selection ---------------------------------------------
    double playhead() const noexcept { return playhead_; }
    void setPlayhead(double t) { playhead_ = std::clamp(t, 0.0, duration_); }

    /// Keeps the playhead visible while playing: pages the view when it runs
    /// off the right edge. Returns true when the view moved.
    bool followPlayhead()
    {
        if (playhead_ >= viewStart_ && playhead_ <= viewEnd()) return false;
        setViewStart(playhead_ - viewLength_ * 0.1);
        return true;
    }

    bool hasSelection() const noexcept { return selEnd_ > selStart_; }
    double selectionStart() const noexcept { return selStart_; }
    double selectionEnd() const noexcept { return selEnd_; }
    void setSelection(double a, double b)
    {
        if (b < a) std::swap(a, b);
        selStart_ = std::clamp(a, 0.0, duration_);
        selEnd_ = std::clamp(b, 0.0, duration_);
    }
    void clearSelection() { selStart_ = selEnd_ = 0.0; }

    // ---- annotations -------------------------------------------------------
    const std::vector<Annotation>& annotations() const noexcept { return annotations_; }

    /// Annotates the selection, or the playhead as a point marker when there
    /// is no selection. Returns the new annotation's id, or -1 for empty text.
    int addAnnotation(const juce::String& text)
    {
        const auto t = text.trim();
        if (t.isEmpty()) return -1;
        Annotation a;
        a.id = nextId_++;
        a.startSec = hasSelection() ? selStart_ : playhead_;
        a.endSec = hasSelection() ? selEnd_ : playhead_;
        a.text = t;
        annotations_.push_back(a);
        sortAnnotations();
        return a.id;
    }

    bool removeAnnotation(int id)
    {
        const auto before = annotations_.size();
        annotations_.erase(
            std::remove_if(annotations_.begin(), annotations_.end(), [id](const Annotation& a) { return a.id == id; }),
            annotations_.end());
        return annotations_.size() != before;
    }

    /// The annotation under `t` (point markers match within `toleranceSec`).
    int annotationAt(double t, double toleranceSec) const
    {
        for (const auto& a : annotations_)
            if (t >= a.startSec - toleranceSec && t <= a.endSec + toleranceSec) return a.id;
        return -1;
    }

    /// Next/previous annotation start after/before `t` (for keyboard jumps).
    double nextAnnotationAfter(double t) const
    {
        for (const auto& a : annotations_)
            if (a.startSec > t + 1e-6) return a.startSec;
        return t;
    }
    double previousAnnotationBefore(double t) const
    {
        double best = t;
        for (const auto& a : annotations_)
            if (a.startSec < t - 1e-6) best = a.startSec;
        return best;
    }

    juce::var annotationsToVar() const
    {
        juce::Array<juce::var> arr;
        for (const auto& a : annotations_) {
            auto* o = new juce::DynamicObject();
            o->setProperty("start_sec", a.startSec);
            o->setProperty("end_sec", a.endSec);
            o->setProperty("text", a.text);
            arr.add(juce::var(o));
        }
        return arr;
    }

    int annotationsFromVar(const juce::var& v)
    {
        annotations_.clear();
        if (auto* arr = v.getArray())
            for (const auto& row : *arr) {
                Annotation a;
                a.id = nextId_++;
                a.startSec = static_cast<double>(row["start_sec"]);
                a.endSec = std::max(a.startSec, static_cast<double>(row["end_sec"]));
                a.text = row["text"].toString().trim();
                if (a.text.isNotEmpty() && a.startSec >= 0.0) annotations_.push_back(a);
            }
        sortAnnotations();
        return static_cast<int>(annotations_.size());
    }

  private:
    void sortAnnotations()
    {
        std::stable_sort(annotations_.begin(), annotations_.end(),
                         [](const Annotation& a, const Annotation& b) { return a.startSec < b.startSec; });
    }

    double duration_ = 0.0;
    double viewStart_ = 0.0, viewLength_ = 0.0;
    double playhead_ = 0.0;
    double selStart_ = 0.0, selEnd_ = 0.0;
    std::vector<Annotation> annotations_;
    int nextId_ = 1;
};

} // namespace khensu
