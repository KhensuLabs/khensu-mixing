// desktop-app/Source/ui/VocalWorkspace/WordClipGainModel.h
// Pure model behind per-word clip-gain drawing: one gain value per
// transcribed word, set by "drawing" a line across the waveform (every word
// the stroke passes takes the stroke's level) or by keyboard nudges. Emits
// the regions list for POST /render/word_clip_gain.
#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace khensu {

class WordClipGainModel {
  public:
    struct Word {
        juce::String text;
        double startSec = 0.0, endSec = 0.0;
        double gainDb = 0.0;
    };
    struct Region {
        double startSec = 0.0, endSec = 0.0, gainDb = 0.0;
    };

    static constexpr double kMaxGainDb = 12.0; // drawing range shown in the UI

    void setWords(std::vector<Word> words)
    {
        words_.clear();
        for (auto& w : words)
            if (w.endSec > w.startSec) words_.push_back(std::move(w));
        std::sort(words_.begin(), words_.end(), [](const Word& a, const Word& b) { return a.startSec < b.startSec; });
    }

    const std::vector<Word>& words() const noexcept { return words_; }
    bool empty() const noexcept { return words_.empty(); }

    int wordAt(double timeSec) const noexcept
    {
        for (size_t i = 0; i < words_.size(); ++i)
            if (timeSec >= words_[i].startSec && timeSec <= words_[i].endSec) return static_cast<int>(i);
        return -1;
    }

    bool setGain(size_t index, double gainDb)
    {
        if (index >= words_.size()) return false;
        words_[index].gainDb = snap(juce::jlimit(-kMaxGainDb, kMaxGainDb, gainDb));
        return true;
    }

    bool nudge(size_t index, double deltaDb)
    {
        return index < words_.size() && setGain(index, words_[index].gainDb + deltaDb);
    }

    /// A drawing stroke from (t0, g0) to (t1, g1): every word whose centre
    /// lies inside the swept time span takes the stroke's interpolated level
    /// at that centre. Returns how many words changed.
    int drawStroke(double t0, double g0, double t1, double g1)
    {
        if (t1 < t0) {
            std::swap(t0, t1);
            std::swap(g0, g1);
        }
        int changed = 0;
        for (size_t i = 0; i < words_.size(); ++i) {
            const double c = 0.5 * (words_[i].startSec + words_[i].endSec);
            const bool hit = (t1 - t0) < 1e-9 ? wordAt(t0) == static_cast<int>(i) : (c >= t0 && c <= t1);
            if (!hit) continue;
            const double frac = (t1 - t0) < 1e-9 ? 0.0 : (c - t0) / (t1 - t0);
            const double before = words_[i].gainDb;
            setGain(i, g0 + (g1 - g0) * frac);
            if (std::abs(words_[i].gainDb - before) > 1e-9) ++changed;
        }
        return changed;
    }

    void resetAll()
    {
        for (auto& w : words_)
            w.gainDb = 0.0;
    }

    /// Non-zero words as render regions; adjacent words with the same gain
    /// merge into one region so the renderer doesn't dip between them.
    std::vector<Region> regions() const
    {
        std::vector<Region> out;
        for (const auto& w : words_) {
            if (std::abs(w.gainDb) < 0.05) continue;
            if (!out.empty() && std::abs(out.back().gainDb - w.gainDb) < 1e-9 && w.startSec - out.back().endSec < 0.05)
                out.back().endSec = w.endSec;
            else
                out.push_back({w.startSec, w.endSec, w.gainDb});
        }
        return out;
    }

    juce::var regionsAsVar() const
    {
        juce::Array<juce::var> arr;
        for (const auto& r : regions()) {
            auto* o = new juce::DynamicObject();
            o->setProperty("start_sec", r.startSec);
            o->setProperty("end_sec", r.endSec);
            o->setProperty("gain_db", r.gainDb);
            arr.add(juce::var(o));
        }
        return arr;
    }

  private:
    static double snap(double db) { return std::round(db * 2.0) / 2.0; } // 0.5 dB steps

    std::vector<Word> words_;
};

} // namespace khensu
