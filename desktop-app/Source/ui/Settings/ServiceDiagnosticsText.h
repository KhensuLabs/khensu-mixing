// desktop-app/Source/ui/Settings/ServiceDiagnosticsText.h
// AUTO-4: the wording of Settings > Data's service diagnostics - memory,
// queues, security events, the analysis cache, the model registry and the
// genre examples - from the service's JSON (read through api::fields).
// Header-only and pure, so tests word a reply without a screen.
#pragma once
#include <JuceHeader.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace khensu::service_diagnostics {

/// 1536 -> "1.5 KB", 48'000'000 -> "45.8 MB".
inline juce::String bytesText(double bytes)
{
    if (!std::isfinite(bytes) || bytes < 0.0) return "-";
    if (bytes < 1024.0) return juce::String(juce::roundToInt(bytes)) + " B";
    if (bytes < 1024.0 * 1024.0) return juce::String(bytes / 1024.0, 1) + " KB";
    if (bytes < 1024.0 * 1024.0 * 1024.0) return juce::String(bytes / (1024.0 * 1024.0), 1) + " MB";
    return juce::String(bytes / (1024.0 * 1024.0 * 1024.0), 2) + " GB";
}

inline double num(const juce::var& v) { return v.isVoid() || v.isUndefined() ? 0.0 : static_cast<double>(v); }

/// GET /diagnostics/memory -> two lines: the process and caches, the pools and the gate.
inline juce::StringArray memoryLines(const juce::var& v)
{
    namespace F = api::fields::MemoryResponse;
    juce::StringArray out;
    juce::String first = TRANS("Memory:") + " ";
    const auto rss = v[F::rss_bytes];
    first << (rss.isVoid() ? TRANS("resident size unknown") : bytesText(num(rss)) + " " + TRANS("resident"));
    const int caches = v[F::result_caches].getArray() != nullptr ? v[F::result_caches].getArray()->size() : 0;
    first << "; " << TRANS("result caches") << " " << bytesText(num(v[F::result_cache_bytes])) << " / "
          << bytesText(num(v[F::result_cache_budget_bytes])) << " (" << caches << ")";
    first << "; " << TRANS("decoded audio") << " " << bytesText(num(v[F::decoded_audio_bytes])) << " / "
          << bytesText(num(v[F::decoded_audio_budget_bytes])) << " ("
          << juce::String(juce::roundToInt(num(v[F::decoded_audio_entries]))) << " " << TRANS("files") << ")";
    out.add(first);

    juce::String second = TRANS("Workers:") + " ";
    juce::StringArray pools;
    if (auto* arr = v[F::pools].getArray())
        for (const auto& p : *arr)
            pools.add(p[api::fields::PoolInfo::name].toString() + " " +
                      juce::String(juce::roundToInt(num(p[api::fields::PoolInfo::workers]))));
    second << (pools.isEmpty() ? juce::String("-") : pools.joinIntoString(", "));
    second << " " << TRANS("on") << " " << juce::String(juce::roundToInt(num(v[F::cpu_count]))) << " " << TRANS("CPUs");
    if (const auto gate = v[F::memory_gate]; gate.isObject()) {
        namespace G = api::fields::MemoryGateInfo;
        if (static_cast<bool>(gate[G::enabled]))
            second << "; " << TRANS("memory gate") << " " << bytesText(num(gate[G::in_use_bytes])) << " / "
                   << bytesText(num(gate[G::budget_bytes])) << ", "
                   << juce::String(juce::roundToInt(num(gate[G::waits]))) << " " << TRANS("waits") << ", "
                   << juce::String(juce::roundToInt(num(gate[G::overcommits]))) << " " << TRANS("overcommits");
        else
            second << "; " << TRANS("memory gate off");
    }
    out.add(second);
    return out;
}

/// GET /diagnostics/queues -> one line: the busy queues (or "idle") and the LLM spend.
inline juce::StringArray queueLines(const juce::var& v)
{
    namespace F = api::fields::QueueDiagnosticsResponse;
    namespace Q = api::fields::QueueMetrics;
    juce::StringArray out;
    juce::StringArray busy;
    int queues = 0;
    if (auto* arr = v[F::queues].getArray())
        for (const auto& q : *arr) {
            ++queues;
            const int active = juce::roundToInt(num(q[Q::active]));
            if (active <= 0) continue;
            juce::String s = q[Q::name].toString() + " " + juce::String(active);
            if (!q[Q::max_active].isVoid()) s << "/" << juce::String(juce::roundToInt(num(q[Q::max_active])));
            s << " (" << TRANS("oldest") << " " << juce::String(juce::roundToInt(num(q[Q::oldest_active_age_sec])))
              << " s)";
            busy.add(s);
        }
    out.add(TRANS("Job queues:") + " " +
            (busy.isEmpty() ? TRANS("all idle") + " (" + juce::String(queues) + ")" : busy.joinIntoString(", ")));
    // llm_budget is a free-form provider -> usage map; show its scalar entries.
    juce::StringArray spend;
    if (auto* obj = v[F::llm_budget].getDynamicObject())
        for (const auto& p : obj->getProperties()) {
            const auto& val = p.value;
            if (val.isDouble() || val.isInt() || val.isInt64())
                spend.add(p.name.toString() + " " + juce::String(static_cast<double>(val), 2));
            else if (auto* inner = val.getDynamicObject()) {
                juce::StringArray parts;
                for (const auto& ip : inner->getProperties())
                    if (ip.value.isDouble() || ip.value.isInt() || ip.value.isInt64())
                        parts.add(ip.name.toString() + " " + juce::String(static_cast<double>(ip.value), 2));
                if (!parts.isEmpty()) spend.add(p.name.toString() + " (" + parts.joinIntoString(", ") + ")");
            }
            if (spend.size() >= 4) break;
        }
    out.add(TRANS("Cloud LLM use (24 h):") + " " + (spend.isEmpty() ? TRANS("none") : spend.joinIntoString("; ")));
    return out;
}

/// GET /diagnostics/security_events -> the count, any anomaly, the latest few events.
inline juce::StringArray securityLines(const juce::var& v, int maxEvents = 3)
{
    namespace F = api::fields::SecurityEventsResponse;
    juce::StringArray out;
    const int count = juce::roundToInt(num(v[F::count]));
    juce::String line = TRANS("Security events:") + " " + juce::String(count);
    juce::StringArray anomalies;
    if (auto* obj = v[F::anomalies].getDynamicObject())
        for (const auto& p : obj->getProperties())
            anomalies.add(p.name.toString() + " x" + juce::String(juce::roundToInt(num(p.value))));
    if (!anomalies.isEmpty())
        line << " - " << TRANS("repeated in the last hour:") << " " << anomalies.joinIntoString(", ");
    out.add(line);
    // Each event is a free-form {ts, kind, detail} record (security_log.py).
    if (auto* arr = v[F::events].getArray())
        for (int i = 0; i < juce::jmin(maxEvents, arr->size()); ++i) {
            const auto& e = (*arr)[i];
            const auto when = juce::Time(static_cast<juce::int64>(num(e["ts"]) * 1000.0));
            out.add("  " + when.formatted("%Y-%m-%d %H:%M") + "  " + e["kind"].toString() + "  " +
                    e["detail"].toString().substring(0, 120));
        }
    return out;
}

/// GET /analysis/cache -> "Analysis cache: 120 results, 45.8 MB of 512.0 MB; 30 hits, 4 misses".
inline juce::String cacheLine(const juce::var& v)
{
    namespace F = api::fields::AnalysisCacheStatsResponse;
    if (!static_cast<bool>(v[F::enabled])) return TRANS("Analysis cache: off");
    return TRANS("Analysis cache:") + " " + juce::String(juce::roundToInt(num(v[F::entries]))) + " " +
           TRANS("results") + ", " + bytesText(num(v[F::bytes])) + " " + TRANS("of") + " " +
           bytesText(num(v[F::budget_bytes])) + "; " + juce::String(juce::roundToInt(num(v[F::hits]))) + " " +
           TRANS("hits") + ", " + juce::String(juce::roundToInt(num(v[F::misses]))) + " " + TRANS("misses");
}

/// GET /analyze/models/registry -> "Model registry: 6 models (MIT 3, CC BY 4.0 2, ...)".
inline juce::String registryLine(const juce::var& v)
{
    namespace F = api::fields::ModelRegistryResponse;
    std::vector<std::pair<juce::String, int>> licences;
    int n = 0;
    if (auto* obj = v[F::models].getDynamicObject())
        for (const auto& p : obj->getProperties()) {
            ++n;
            auto lic = p.value[api::fields::ModelRegistryEntry::license].toString().trim();
            if (lic.isEmpty()) lic = TRANS("licence not stated");
            auto it = std::find_if(licences.begin(), licences.end(), [&](const auto& e) { return e.first == lic; });
            if (it == licences.end())
                licences.emplace_back(lic, 1);
            else
                ++it->second;
        }
    if (n == 0) return TRANS("Model registry: no models listed");
    std::stable_sort(licences.begin(), licences.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    juce::StringArray parts;
    for (const auto& [lic, count] : licences)
        parts.add(lic + " " + juce::String(count));
    return TRANS("Model registry:") + " " + juce::String(n) + " " + (n == 1 ? TRANS("model") : TRANS("models")) + " (" +
           parts.joinIntoString(", ") + ")";
}

/// The names of the registry's models, for a tooltip.
inline juce::String registryTooltip(const juce::var& v)
{
    namespace F = api::fields::ModelRegistryResponse;
    juce::StringArray lines;
    if (auto* obj = v[F::models].getDynamicObject())
        for (const auto& p : obj->getProperties()) {
            namespace E = api::fields::ModelRegistryEntry;
            juce::String line = p.name.toString();
            if (const auto lic = p.value[E::license].toString(); lic.isNotEmpty()) line << " - " << lic;
            if (const auto by = p.value[E::produced_by].toString(); by.isNotEmpty()) line << " - " << by;
            lines.add(line);
        }
    if (const auto note = v[F::note].toString(); note.isNotEmpty()) lines.add(note);
    return lines.joinIntoString("\n");
}

/// GET /genre/examples -> "Your genre examples: 12 (rock 5, metal 4, pop 3)".
inline juce::String genreExamplesLine(const juce::var& v)
{
    namespace F = api::fields::GenreExamplesSummaryResponse;
    const int total = juce::roundToInt(num(v[F::examples]));
    if (total <= 0) return TRANS("No genre examples stored yet.");
    std::vector<std::pair<juce::String, int>> perGenre;
    if (auto* obj = v[F::genres].getDynamicObject())
        for (const auto& p : obj->getProperties())
            perGenre.emplace_back(p.name.toString(), juce::roundToInt(num(p.value)));
    std::stable_sort(perGenre.begin(), perGenre.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    juce::StringArray parts;
    for (const auto& [genre, count] : perGenre)
        parts.add(genre + " " + juce::String(count));
    return TRANS("Your genre examples:") + " " + juce::String(total) +
           (parts.isEmpty() ? juce::String() : " (" + parts.joinIntoString(", ") + ")");
}

} // namespace khensu::service_diagnostics
