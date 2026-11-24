#include <khensu/targets/GenreSpread.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

namespace khensu {

namespace {

std::optional<GenreSpread::Percentiles> percentilesOf(const juce::var& v)
{
    if (!v.isObject()) return std::nullopt;
    static const char* kKeys[5] = {"p10", "p25", "p50", "p75", "p90"};
    GenreSpread::Percentiles p{};
    for (size_t i = 0; i < 5; ++i) {
        const auto x = v.getProperty(kKeys[i], juce::var());
        if (!(x.isDouble() || x.isInt() || x.isInt64())) return std::nullopt;
        p[i] = static_cast<float>(static_cast<double>(x));
    }
    return p;
}

GenreSpread::Entry entryOf(const std::string& id, const juce::var& v, size_t bands)
{
    GenreSpread::Entry e;
    e.genreId = id;
    e.tracks = static_cast<int>(v.getProperty("n", 0));
    if (const auto bandObj = v.getProperty("band_db", juce::var()); bandObj.isObject()) {
        static const char* kKeys[5] = {"p10", "p25", "p50", "p75", "p90"};
        e.bandDb.assign(bands, std::nullopt);
        for (size_t b = 0; b < bands; ++b) {
            GenreSpread::Percentiles p{};
            bool ok = true;
            for (size_t i = 0; i < 5 && ok; ++i) {
                const auto* arr = bandObj.getProperty(kKeys[i], juce::var()).getArray();
                if (arr == nullptr || static_cast<size_t>(arr->size()) <= b) { ok = false; break; }
                const auto& x = arr->getReference(static_cast<int>(b));
                if (x.isVoid() || x.isUndefined()) { ok = false; break; }
                p[i] = static_cast<float>(static_cast<double>(x));
            }
            if (ok) e.bandDb[b] = p;
        }
    }
    if (const auto* metrics = v.getProperty("metrics", juce::var()).getDynamicObject())
        for (const auto& prop : metrics->getProperties())
            if (const auto* arr = prop.value.getArray()) {
                std::vector<float> vals;
                vals.reserve(static_cast<size_t>(arr->size()));
                for (const auto& x : *arr)
                    vals.push_back(static_cast<float>(static_cast<double>(x)));
                std::sort(vals.begin(), vals.end());
                e.metrics[prop.name.toString().toStdString()] = std::move(vals);
            }
    if (const auto* sc = v.getProperty("scorecard", juce::var()).getDynamicObject())
        for (const auto& prop : sc->getProperties())
            if (const auto p = percentilesOf(prop.value)) e.scorecard[prop.name.toString().toStdString()] = *p;
    return e;
}

} // namespace

const std::vector<float>* GenreSpread::Entry::metric(const std::string& name) const
{
    const auto it = metrics.find(name);
    return it != metrics.end() && !it->second.empty() ? &it->second : nullptr;
}

std::optional<float> GenreSpread::Entry::fractionBelow(const std::string& metricName, float value) const
{
    const auto* v = metric(metricName);
    if (v == nullptr || !std::isfinite(value)) return std::nullopt;
    const auto lo = std::lower_bound(v->begin(), v->end(), value);
    const auto hi = std::upper_bound(v->begin(), v->end(), value);
    const double below = static_cast<double>(lo - v->begin()) + 0.5 * static_cast<double>(hi - lo);
    return static_cast<float>(below / static_cast<double>(v->size()));
}

bool GenreSpread::Entry::hasBands() const noexcept
{
    return std::count_if(bandDb.begin(), bandDb.end(), [](const auto& b) { return b.has_value(); }) >= 4;
}

const GenreSpread::Entry* GenreSpread::find(const std::string& genreId) const
{
    if (!genreId.empty())
        if (const auto it = genres.find(genreId); it != genres.end()) return &it->second;
    return all ? &*all : nullptr;
}

GenreSpread GenreSpread::fromJson(const std::string& json)
{
    GenreSpread s;
    const auto root = juce::JSON::parse(juce::String(juce::CharPointer_UTF8(json.c_str())));
    if (!root.isObject()) return s;
    if (const auto* hz = root.getProperty("band_hz", juce::var()).getArray())
        for (const auto& x : *hz)
            s.bandHz.push_back(static_cast<float>(static_cast<double>(x)));
    if (const auto* genres = root.getProperty("genres", juce::var()).getDynamicObject())
        for (const auto& prop : genres->getProperties()) {
            const auto id = prop.name.toString().toStdString();
            s.genres[id] = entryOf(id, prop.value, s.bandHz.size());
        }
    if (const auto every = root.getProperty("all", juce::var()); every.isObject())
        s.all = entryOf("", every, s.bandHz.size());
    return s;
}

GenreSpread GenreSpread::fromFile(const std::string& path)
{
    const juce::File f(juce::String(juce::CharPointer_UTF8(path.c_str())));
    if (!f.existsAsFile()) return {};
    return fromJson(f.loadFileAsString().toStdString());
}

} // namespace khensu
