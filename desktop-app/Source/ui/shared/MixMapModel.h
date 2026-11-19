// desktop-app/Source/ui/shared/MixMapModel.h
// Data model for the unified mix-map canvas: stems as nodes placed by where
// they live in the spectrum, sized by their share of the mix's energy (at a
// chosen moment or on average), joined by masking edges whose weight is the
// pair's collision score (at that moment, when windowed scores exist).
// Pure logic - unit-tested in tests/ui/MixMapModelTest.cpp.
#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace khensu {

class MixMapModel {
  public:
    struct Window {
        double startSec = 0.0, endSec = 0.0, score = 0.0;
    };
    struct Pair {
        juce::String a, b;
        double score = 0.0;             ///< whole-song collision, 0..1
        std::vector<double> bandScores; ///< per band, same order as bandsHz
        std::vector<Window> windows;    ///< collision localized in time
    };
    struct EnergyFrame {
        double timeSec = 0.0;
        std::map<juce::String, double> fractions; ///< sums to ~1
    };
    struct Node {
        juce::String stem;
        double centreHz = 0.0; ///< spectral position (log-x on the canvas)
        double energy = 0.0;   ///< 0..1 share of mix energy
    };
    struct Edge {
        juce::String a, b;
        double score = 0.0;   ///< collision at the chosen time (or overall)
        double clashHz = 0.0; ///< band where the two collide most
    };

    void setMasking(std::vector<Pair> pairs, std::vector<double> bandsHz)
    {
        pairs_ = std::move(pairs);
        bandsHz_ = std::move(bandsHz);
    }
    void setEnergy(std::vector<EnergyFrame> frames)
    {
        frames_ = std::move(frames);
        std::sort(frames_.begin(), frames_.end(),
                  [](const EnergyFrame& x, const EnergyFrame& y) { return x.timeSec < y.timeSec; });
    }

    bool empty() const noexcept { return pairs_.empty() && frames_.empty(); }
    const std::vector<EnergyFrame>& energyFrames() const noexcept { return frames_; }
    double durationSec() const noexcept { return frames_.empty() ? 0.0 : frames_.back().timeSec; }

    /// Stems known from either source, in a stable low-to-high order.
    juce::StringArray stems() const
    {
        juce::StringArray s;
        for (const auto& p : pairs_) {
            s.addIfNotAlreadyThere(p.a);
            s.addIfNotAlreadyThere(p.b);
        }
        for (const auto& f : frames_)
            for (const auto& [name, frac] : f.fractions)
                s.addIfNotAlreadyThere(name);
        std::vector<juce::String> v(s.begin(), s.end());
        std::stable_sort(v.begin(), v.end(),
                         [](const juce::String& x, const juce::String& y) { return canonicalHz(x) < canonicalHz(y); });
        return juce::StringArray(v.data(), static_cast<int>(v.size()));
    }

    /// Canonical spectral home for a stem name (fallback position).
    static double canonicalHz(const juce::String& stem)
    {
        const auto s = stem.toLowerCase();
        if (s.contains("bass")) return 80.0;
        if (s.contains("kick")) return 60.0;
        if (s.contains("drum")) return 300.0;
        if (s.contains("vocal") || s.contains("vox")) return 1500.0;
        if (s.contains("other")) return 800.0;
        return 1000.0;
    }

    /// Energy fraction per stem at `timeSec` (nearest frame), or the mean
    /// over the song when timeSec < 0.
    std::map<juce::String, double> energyAt(double timeSec) const
    {
        std::map<juce::String, double> out;
        if (frames_.empty()) return out;
        if (timeSec < 0.0) {
            for (const auto& f : frames_)
                for (const auto& [name, frac] : f.fractions)
                    out[name] += frac;
            for (auto& [name, v] : out)
                v /= static_cast<double>(frames_.size());
            return out;
        }
        const auto it = std::lower_bound(frames_.begin(), frames_.end(), timeSec,
                                         [](const EnergyFrame& f, double t) { return f.timeSec < t; });
        const auto& f = it == frames_.end() ? frames_.back() : *it;
        return f.fractions;
    }

    /// Collision score of pair at `timeSec` from its windows (falls back to
    /// the whole-song score when the pair has no window covering the time,
    /// or when timeSec < 0).
    static double scoreAt(const Pair& p, double timeSec)
    {
        if (timeSec < 0.0) return p.score;
        for (const auto& w : p.windows)
            if (timeSec >= w.startSec && timeSec < w.endSec) return w.score;
        return p.score;
    }

    std::vector<Node> nodes(double timeSec = -1.0) const
    {
        const auto energy = energyAt(timeSec);
        std::vector<Node> out;
        for (const auto& stem : stems()) {
            Node n;
            n.stem = stem;
            n.centreHz = spectralCentre(stem);
            const auto it = energy.find(stem);
            n.energy = it == energy.end() ? 0.0 : juce::jlimit(0.0, 1.0, it->second);
            out.push_back(n);
        }
        return out;
    }

    std::vector<Edge> edges(double timeSec = -1.0, double minScore = 0.0) const
    {
        std::vector<Edge> out;
        for (const auto& p : pairs_) {
            const double s = scoreAt(p, timeSec);
            if (s < minScore) continue;
            out.push_back({p.a, p.b, s, clashBandHz(p)});
        }
        std::sort(out.begin(), out.end(), [](const Edge& x, const Edge& y) { return x.score > y.score; });
        return out;
    }

    /// Severity bucket shared with the masking panels: 3 severe, 2 moderate,
    /// 1 minor, 0 negligible.
    static int severity(double score)
    {
        if (score >= 0.6) return 3;
        if (score >= 0.4) return 2;
        if (score >= 0.2) return 1;
        return 0;
    }

  private:
    double clashBandHz(const Pair& p) const
    {
        if (p.bandScores.empty() || bandsHz_.empty()) return 0.0;
        const auto it = std::max_element(p.bandScores.begin(), p.bandScores.end());
        const auto i = static_cast<size_t>(std::distance(p.bandScores.begin(), it));
        if (i >= bandsHz_.size()) return 0.0;
        // Band lower edge -> geometric centre with the next edge when known.
        const double lo = bandsHz_[i];
        const double hi = i + 1 < bandsHz_.size() ? bandsHz_[i + 1] : lo * 2.0;
        return std::sqrt(std::max(1.0, lo) * hi);
    }

    /// Where the stem sits spectrally: the score-weighted log-mean of the
    /// bands it collides in, blended with its canonical home so a stem with
    /// little masking still lands somewhere sensible.
    double spectralCentre(const juce::String& stem) const
    {
        double wsum = 0.0, logSum = 0.0;
        for (const auto& p : pairs_) {
            if (p.a != stem && p.b != stem) continue;
            for (size_t i = 0; i < p.bandScores.size() && i < bandsHz_.size(); ++i) {
                const double hi = i + 1 < bandsHz_.size() ? bandsHz_[i + 1] : bandsHz_[i] * 2.0;
                const double c = std::sqrt(std::max(1.0, bandsHz_[i]) * hi);
                wsum += p.bandScores[i];
                logSum += p.bandScores[i] * std::log(c);
            }
        }
        const double home = canonicalHz(stem);
        if (wsum <= 1e-9) return home;
        return std::exp(0.5 * logSum / wsum + 0.5 * std::log(home));
    }

    std::vector<Pair> pairs_;
    std::vector<double> bandsHz_;
    std::vector<EnergyFrame> frames_;
};

} // namespace khensu
