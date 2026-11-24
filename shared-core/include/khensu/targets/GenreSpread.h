// shared-core/include/khensu/targets/GenreSpread.h
// VIZ-1: where released tracks of a genre actually sit - the measured spread
// behind the charts (resources/genre_norms/genre_spread.json, written by
// scripts/build_genre_spread.py from the corpus the app's own AudioAnalyzer
// measured). A genre target is one window; the spread shows the music:
//   bandDb     - p10..p90 of the normalised 1/6-octave band power per band
//                (khensu/analysis/BandPower.h's grid), the tonal chart's bands;
//   metrics    - every measured track's value per metric (sorted), the tiles'
//                dot strips and percentile ("louder than 92 %");
//   scorecard  - p10..p90 of each readiness category on released tracks.
// A genre with too few tracks has no entry; callers fall back to "all".
// THREAD_CONTEXT(Any) - load once, then read-only.
#pragma once
#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace khensu {

struct GenreSpread {
    /// The five percentiles, p10 / p25 / p50 / p75 / p90.
    using Percentiles = std::array<float, 5>;
    enum PercentileIndex { kP10 = 0, kP25, kP50, kP75, kP90 };

    struct Entry {
        std::string genreId; ///< "" for the whole corpus
        int tracks = 0;
        /// One per band of bandHz; a band without enough tracks has nullopt.
        std::vector<std::optional<Percentiles>> bandDb;
        std::map<std::string, std::vector<float>> metrics; ///< sorted values
        std::map<std::string, Percentiles> scorecard;     ///< category id (and "overall")

        const std::vector<float>* metric(const std::string& name) const;
        /// Share (0..1) of released tracks strictly below `value` (ties count
        /// half); nullopt without that metric.
        std::optional<float> fractionBelow(const std::string& metricName, float value) const;
        bool hasBands() const noexcept;
    };

    std::vector<float> bandHz;
    std::map<std::string, Entry> genres;
    std::optional<Entry> all;

    bool empty() const noexcept { return genres.empty() && !all.has_value(); }
    /// The genre's entry, or the whole corpus when the genre has none (or id
    /// is empty). nullptr when nothing is loaded.
    const Entry* find(const std::string& genreId) const;

    /// Parses genre_spread.json; an empty GenreSpread on any error.
    static GenreSpread fromJson(const std::string& json);
    static GenreSpread fromFile(const std::string& path);
};

} // namespace khensu
