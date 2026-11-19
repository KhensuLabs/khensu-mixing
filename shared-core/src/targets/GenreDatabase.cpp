// THREAD_CONTEXT(WorkerThread) for load*; THREAD_CONTEXT(AnyThread) for queries.
#include <khensu/targets/GenreDatabase.h>

#include <khensu/common/AppPaths.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

namespace khensu {

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static std::string readFileToString(const std::string& filePath)
{
    // Paths are UTF-8 (AppController builds them from juce::File). MSVC's
    // std::ifstream(std::string) interprets them in the ANSI code page, so a
    // user folder with non-ASCII characters silently failed to load.
    const juce::File file(juce::String::fromUTF8(filePath.c_str()));
    if (!file.existsAsFile()) return {};
    return file.loadFileAsString().toStdString();
}

Result<void> GenreDatabase::loadFromString(const std::string& jsonContent) { return parseJson(jsonContent); }

Result<void> GenreDatabase::loadFromFile(const std::string& filePath)
{
    std::string baseContent = readFileToString(filePath);
    if (baseContent.empty()) return Result<void>::err("Genre database file not found: " + filePath);

    // User-extensible layer: merge <data_dir>/genre_targets_custom.json over
    // the bundled base catalog. Custom entries override by id; new ids append.
    juce::var base = juce::JSON::parse(juce::String(baseContent));
    if (!base.isObject()) return Result<void>::err("Genre database base JSON is not an object.");

    const juce::File customFile(juce::String(AppPaths::dataDirectory()) + "/genre_targets_custom.json");
    if (customFile.existsAsFile()) {
        juce::var custom = juce::JSON::parse(customFile.loadFileAsString());
        if (custom.isObject()) {
            if (auto* baseGenres = base["genres"].getArray()) {
                if (auto* customGenres = custom["genres"].getArray()) {
                    // Build id -> index map for base genres.
                    std::map<juce::String, int> indexById;
                    for (int i = 0; i < baseGenres->size(); ++i) {
                        if (auto* obj = (*baseGenres)[i].getDynamicObject())
                            indexById[obj->getProperty("id").toString()] = i;
                    }
                    for (const auto& cvar : *customGenres) {
                        if (auto* cobj = cvar.getDynamicObject()) {
                            const juce::String cid = cobj->getProperty("id").toString();
                            auto it = indexById.find(cid);
                            if (it != indexById.end() && it->second < baseGenres->size()) {
                                // Merge over the existing genre object.
                                if (auto* baseObj = (*baseGenres)[it->second].getDynamicObject()) {
                                    for (const auto& prop : cobj->getProperties())
                                        baseObj->setProperty(prop.name, prop.value);
                                }
                            } else {
                                baseGenres->add(cvar);
                                indexById[cid] = baseGenres->size() - 1;
                            }
                        }
                    }
                }
            }
            // Bump the merged version upward if the custom file carries one.
            if (base["version"].isDouble() && custom["version"].isDouble()) {
                const double mergedVersion =
                    std::max(static_cast<double>(base["version"]), static_cast<double>(custom["version"]));
                if (auto* baseObj = base.getDynamicObject()) baseObj->setProperty("version", mergedVersion);
            }
        }
    }

    return parseJson(juce::JSON::toString(base).toStdString());
}

std::optional<GenreEntry> GenreDatabase::findById(const std::string& id) const
{
    auto it = m_byId.find(id);
    if (it != m_byId.end()) return it->second;
    return std::nullopt;
}

std::optional<GenreEntry> GenreDatabase::findByName(const std::string& name) const
{
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    auto it = m_byNameLower.find(lower(name));
    if (it != m_byNameLower.end()) return it->second;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

static float getFloat(const juce::var& v, const char* key, float def = 0.0f)
{
    const juce::var val = v[key];
    return val.isUndefined() || val.isVoid() ? def : static_cast<float>(static_cast<double>(val));
}

static std::string getString(const juce::var& v, const char* key) { return v[key].toString().toStdString(); }

/// Reads a `[min, max]` pair into the outputs only when it is well formed
/// (two finite numbers, min <= max); otherwise the defaults stay. Returns
/// whether the pair was applied.
static bool readRange(const juce::var& v, float& outMin, float& outMax)
{
    const auto* arr = v.getArray();
    if (arr == nullptr || arr->size() != 2) return false;
    const auto& lo = (*arr)[0];
    const auto& hi = (*arr)[1];
    if (!(lo.isDouble() || lo.isInt() || lo.isInt64()) || !(hi.isDouble() || hi.isInt() || hi.isInt64())) return false;
    const auto mn = static_cast<float>(static_cast<double>(lo));
    const auto mx = static_cast<float>(static_cast<double>(hi));
    if (!std::isfinite(mn) || !std::isfinite(mx) || mn > mx) {
        juce::Logger::writeToLog("GenreDatabase: ignoring malformed range [" + juce::String(mn) + ", " +
                                 juce::String(mx) + "] - expected two finite numbers with min <= max.");
        return false;
    }
    outMin = mn;
    outMax = mx;
    return true;
}

// Spectral-tilt convention (AUDIT-009). Every built-in tilt target below was
// hand-tuned against the OLD C++ tilt, the slope of mean per-bin power
// (a power DENSITY): pink noise read -3.01 dB/oct and the "typical mix"
// 1/f^1.5 spectrum read -4.5 - exactly the -4.5 baseline used here. The
// analyzer now measures octave-band POWER (the Python dsp_metrics
// definition: pink 0, white +3), which reads every spectrum 10*log10(2) =
// +3.01 dB/oct higher. The targets are shifted by that same constant so each
// recommendation keeps its meaning; explicit JSON values are taken as-is
// (genre_targets.json carries none today) and must be authored in the
// band-power convention.
static constexpr float kTiltDensityToBandPowerDb = 3.0103f;

// Hand-tuned spectral-tilt/crest overrides for genres the audit specifically
// called out (wall-of-sound shoegaze vs transient-forward hyperpop, etc) -
// applied after the heuristic derivation below, which covers every other
// genre in the database without requiring every entry's JSON to be hand-edited.
// Tilt values are written in the original density convention and converted
// with kTiltDensityToBandPowerDb on use.
static const std::map<std::string, std::pair<float, float>>& tiltCrestOverrides()
{
    // id -> { spectral_tilt_target_db_per_octave (density convention), crest_factor_target_db }
    static const std::map<std::string, std::pair<float, float>> overrides = {
        {"shoegaze", {-3.0f, 8.0f}},   // hazy but present highs, dense compressed wall
        {"hyperpop", {-1.0f, 6.0f}},   // exaggerated brightness, heavily limited
        {"dream-pop", {-5.0f, 10.0f}}, // softer/airier highs, moderate dynamics
        {"ambient", {-7.0f, 14.0f}},   // dark, rolled off, very dynamic
        {"dark-ambient", {-8.0f, 15.0f}}, {"lo-fi-hip-hop", {-8.0f, 12.0f}}, // filtered/dark, tape-like dynamics
        {"lo-fi-beats", {-8.0f, 12.0f}},
    };
    return overrides;
}

Result<void> GenreDatabase::parseJson(const std::string& jsonContent)
{
    juce::var root = juce::JSON::parse(juce::String(jsonContent));
    if (!root.isObject()) return Result<void>::err("Genre JSON root is not an object.");

    const juce::var genresVar = root["genres"];
    if (!genresVar.isArray()) return Result<void>::err("Genre JSON missing 'genres' array.");

    m_byId.clear();
    m_byNameLower.clear();
    m_ordered.clear();

    for (int i = 0; i < genresVar.size(); ++i) {
        const juce::var& e = genresVar[i];

        GenreEntry g;
        g.id = getString(e, "id");
        g.name = getString(e, "name");
        g.parent = getString(e, "parent");
        g.notes = getString(e, "notes");

        g.lufs_target = getFloat(e, "lufs_target", -14.0f);
        // Defaults, replaced only by a well-formed [min, max] array (reading
        // the array field as a scalar used to yield 0/0 LUFS for a malformed
        // entry).
        g.lufs_range_min = -16.0f;
        g.lufs_range_max = -11.0f;

        // Array fields like lufs_range: [min, max]. A pair is taken only when
        // both ends are finite and min <= max (CORE-21): a hand-edited custom
        // genre file with an inverted or NaN range used to load as-is, so
        // every value was simultaneously "too loud" and "too quiet".
        readRange(e["lufs_range"], g.lufs_range_min, g.lufs_range_max);
        readRange(e["lra_range"], g.lra_range_min, g.lra_range_max);
        readRange(e["stereo_width_range"], g.stereo_width_min, g.stereo_width_max);
        readRange(e["typical_bpm_range"], g.typical_bpm_min, g.typical_bpm_max);

        g.true_peak_max = getFloat(e, "true_peak_max", -1.0f);
        g.spectral_centroid_hz = getFloat(e, "spectral_centroid_hz", 3000.0f);
        g.low_end_weight = getFloat(e, "low_end_weight", 0.5f);
        g.compression_ratio_typical = getFloat(e, "compression_ratio_typical", 3.0f);
        g.heaviness_distortion_norm = getFloat(e, "heaviness_distortion_norm", 0.2f);
        g.heaviness_weight_norm = getFloat(e, "heaviness_weight_norm", 0.4f);
        g.heaviness_clarity_norm = getFloat(e, "heaviness_clarity_norm", 0.6f);
        g.heaviness_extremity_norm = getFloat(e, "heaviness_extremity_norm", 0.2f);
        g.hook_density_norm = getFloat(e, "hook_density_norm", 0.5f);
        g.chorus_contrast_norm = getFloat(e, "chorus_contrast_norm", 0.5f);
        g.vocal_presence_db = getFloat(e, "vocal_presence_db", 0.0f);

        // Optional explicit stem-energy balance targets.
        if (const auto* obj = e["stem_energy_balance"].getDynamicObject()) {
            for (const auto& prop : obj->getProperties()) {
                const std::string bucket = prop.name.toString().toStdString();
                g.stem_energy_balance_pct[bucket] = static_cast<float>(static_cast<double>(prop.value));
            }
        }

        // Spectral tilt / crest targets: read explicit JSON values if the
        // entry specifies them, otherwise derive a reasonable curve from
        // fields every genre already has (centroid -> brighter/darker tilt,
        // typical compression ratio -> lower/higher crest factor) so every
        // genre gets a real target instead of the same flat default.
        const float derivedTilt =
            std::clamp(-4.5f + (g.spectral_centroid_hz - 3000.0f) / 3000.0f * 2.0f, -9.0f, -1.0f) +
            kTiltDensityToBandPowerDb;
        const float derivedCrest = std::clamp(14.0f - g.compression_ratio_typical * 1.2f, 5.0f, 16.0f);
        g.spectral_tilt_target_db_per_octave = getFloat(e, "spectral_tilt_target_db_per_octave", derivedTilt);
        g.crest_factor_target_db = getFloat(e, "crest_factor_target_db", derivedCrest);

        // Built-in per-genre curve overrides replace only DERIVED values: an
        // explicit JSON (or user custom-file) value always wins - previously
        // the overrides clobbered user customisation of these genres.
        if (auto it = tiltCrestOverrides().find(g.id); it != tiltCrestOverrides().end()) {
            if (!e.hasProperty("spectral_tilt_target_db_per_octave"))
                g.spectral_tilt_target_db_per_octave = it->second.first + kTiltDensityToBandPowerDb;
            if (!e.hasProperty("crest_factor_target_db")) g.crest_factor_target_db = it->second.second;
        }

        if (!g.id.empty()) {
            auto lower = [](std::string s) {
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
                return s;
            };
            m_byId[g.id] = g;
            m_byNameLower[lower(g.name)] = g;
            m_byNameLower[lower(g.id)] = g;
            m_ordered.push_back(std::move(g));
        }
    }

    m_loaded = true;
    return Result<void>::ok();
}

} // namespace khensu
