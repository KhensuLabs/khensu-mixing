// desktop-app/Source/ui/shared/FindingCalibrationCache.cpp
#include "FindingCalibrationCache.h"
#include "FindingEvidence.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include <atomic>
#include <map>
#include <mutex>

namespace khensu {

namespace apif = api::fields;

namespace {
std::mutex& tableMutex()
{
    static std::mutex m;
    return m;
}
std::map<juce::String, FindingCalibrationCache::Entry>& table()
{
    static std::map<juce::String, FindingCalibrationCache::Entry> t;
    return t;
}
std::atomic<bool> gFetched{false};
std::atomic<bool> gInFlight{false};
std::atomic<juce::uint32> gLastAttemptMs{0};
constexpr juce::uint32 kRetryMs = 60000;
} // namespace

std::optional<FindingCalibrationCache::Entry> FindingCalibrationCache::lookup(const juce::String& issueCode)
{
    const std::lock_guard<std::mutex> lock(tableMutex());
    const auto it = table().find(issueCode);
    if (it == table().end()) return std::nullopt;
    return it->second;
}

juce::String FindingCalibrationCache::basisTag(const juce::String& issueCode)
{
    if (const auto e = lookup(issueCode)) return "calibrated (n=" + juce::String(e->n) + ")";
    return "heuristic";
}

juce::String FindingCalibrationCache::basisTooltip(const juce::String& issueCode)
{
    // NEW-2: what the finding was measured to do on released music, beside its basis.
    const auto tested = finding_evidence::describe(issueCode);
    return basisOnly(issueCode) + (tested.isEmpty() ? juce::String() : "\n\n" + tested);
}

juce::String FindingCalibrationCache::basisOnly(const juce::String& issueCode)
{
    if (const auto e = lookup(issueCode))
        return TRANS("Calibrated: measured on %n labelled tracks. When this finding fires it is right %p% of the "
                     "time, and it catches %r% of the real cases.")
            .replace("%n", juce::String(e->n))
            .replace("%p", juce::String(juce::roundToInt(e->precision * 100.0)))
            .replace("%r", juce::String(juce::roundToInt(e->recall * 100.0)));
    return TRANS("Heuristic: an engineering rule with a hand-set threshold. It has not yet been measured against a "
                 "labelled set of mixes, so treat it as an informed opinion, not a verdict.");
}

void FindingCalibrationCache::setFromVar(const juce::var& response)
{
    std::map<juce::String, Entry> next;
    if (static_cast<bool>(response[apif::CalibrationTableResponse::available])) {
        if (auto* codes = response[apif::CalibrationTableResponse::codes].getDynamicObject()) {
            for (const auto& prop : codes->getProperties()) {
                Entry e;
                e.n = static_cast<int>(prop.value[apif::CalibrationCodeSummary::n]);
                e.precision = static_cast<double>(prop.value[apif::CalibrationCodeSummary::precision]);
                e.recall = static_cast<double>(prop.value[apif::CalibrationCodeSummary::recall]);
                if (e.n > 0) next[prop.name.toString()] = e;
            }
        }
    }
    const std::lock_guard<std::mutex> lock(tableMutex());
    table() = std::move(next);
}

void FindingCalibrationCache::ensureFetched(const std::function<void(std::function<void()>)>& runInBackground)
{
    if (gFetched.load() || !runInBackground) return;
    const auto now = juce::Time::getMillisecondCounter();
    const auto last = gLastAttemptMs.load();
    if (last != 0 && now - last < kRetryMs) return;
    bool expected = false;
    if (!gInFlight.compare_exchange_strong(expected, true)) return;
    gLastAttemptMs.store(now == 0 ? 1 : now);
    // Global (not session-scoped) data: the result is stored, never published
    // into a component, so there is no stale-session result to drop.
    runInBackground([] {
        const auto result = ipc::HttpServiceBridge().getCollab("/calibration/findings");
        if (result.is_ok()) {
            setFromVar(result.value());
            gFetched.store(true);
        }
        gInFlight.store(false);
    });
}

void FindingCalibrationCache::resetForTests()
{
    const std::lock_guard<std::mutex> lock(tableMutex());
    table().clear();
    gFetched.store(false);
    gInFlight.store(false);
    gLastAttemptMs.store(0);
}

} // namespace khensu
