// desktop-app/Source/ui/VocalWorkspace/MixTuningReference.h
// AUDIT-016: the vocal intonation panels (Pitch Accuracy, the pitch-editor
// note grid, Recreate Sound's pitched MIDI) measured every take against
// A=440, so an A=432 track read ~32 cents flat on every note and "Retune all"
// pulled the vocal sharp against the band. They now measure against the
// MIX's concert A from /analyze/tuning_reference, fetched once per session
// epoch and shared through this cache.
// THREAD_CONTEXT(AnyThread) - resolve() is called from background jobs.
#pragma once
#include <JuceHeader.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/SessionEpoch.h"
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

/// Once-per-(session epoch, mix file) cache of the mix's tuning reference.
class MixTuningReference {
  public:
    /// Returns the reference in Hz, or nullopt when it could not be measured
    /// or falls outside the service's 400-480 Hz window (callers then send
    /// nothing and the service uses A=440).
    using Fetch = std::function<std::optional<juce::var>(const juce::String& mixPath)>;

    static MixTuningReference& instance()
    {
        static MixTuningReference cache;
        return cache;
    }

    /// The mix reference for `mixPath` under `epoch`. Fetches at most once
    /// per (epoch, path): the lock is held across the fetch so concurrent
    /// panels share one request. A failed fetch (nullopt from `fetch`) is not
    /// cached, so the next caller retries. With no epoch (not on a pool
    /// thread) it fetches without caching.
    std::optional<double> resolve(const juce::String& mixPath, std::optional<uint64_t> epoch, const Fetch& fetch)
    {
        if (mixPath.isEmpty()) return std::nullopt;
        const std::lock_guard<std::mutex> lock(mutex_);
        if (epoch.has_value() && valid_ && *epoch == epoch_ && mixPath == path_) return referenceHz_;
        const auto response = fetch(mixPath);
        if (!response.has_value()) return std::nullopt;
        const auto hz = parseReferenceHz(*response);
        if (epoch.has_value()) {
            epoch_ = *epoch;
            path_ = mixPath;
            referenceHz_ = hz;
            valid_ = true;
        }
        return hz;
    }

    /// Records a reference measured elsewhere (the Tuning Reference panel).
    void store(const juce::String& mixPath, std::optional<uint64_t> epoch, const juce::var& response)
    {
        if (!epoch.has_value() || mixPath.isEmpty()) return;
        const std::lock_guard<std::mutex> lock(mutex_);
        epoch_ = *epoch;
        path_ = mixPath;
        referenceHz_ = parseReferenceHz(response);
        valid_ = true;
    }

    void invalidate()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        valid_ = false;
        referenceHz_.reset();
    }

    /// reference_hz of a /analyze/tuning_reference response, if usable.
    static std::optional<double> parseReferenceHz(const juce::var& response)
    {
        const juce::var ref = response[api::fields::TuningReferenceResponse::reference_hz];
        if (!ref.isDouble() && !ref.isInt() && !ref.isInt64()) return std::nullopt;
        return ipc::HttpServiceBridge::usableReferenceHz(static_cast<double>(ref));
    }

    /// The usual call from a panel's background job: the mix reference for
    /// the job's session epoch, fetched through `bridge` on a cache miss.
    static std::optional<double> forJob(ipc::HttpServiceBridge& bridge, const juce::String& mixPath)
    {
        return instance().resolve(mixPath, session_epoch::current(),
                                  [&bridge](const juce::String& p) -> std::optional<juce::var> {
                                      auto r = bridge.analyzeTuningReference(p.toStdString());
                                      if (r.is_err()) return std::nullopt;
                                      return r.value();
                                  });
    }

  private:
    std::mutex mutex_;
    bool valid_ = false;
    uint64_t epoch_ = 0;
    juce::String path_;
    std::optional<double> referenceHz_;
};

// fromResponse() parses both /analyze/pitch_accuracy and /analyze/pitch_curve
// through the PitchAccuracyResponse names; these pin that pitch_curve still
// carries the same fields (a rename on either side is a compile error).
namespace tuning_fields_detail {
namespace pa = api::fields::PitchAccuracyResponse;
namespace pc = api::fields::PitchCurveResponse;
static_assert(std::string_view(pa::reference_hz) == pc::reference_hz);
static_assert(std::string_view(pa::measurement_reference_hz) == pc::measurement_reference_hz);
static_assert(std::string_view(pa::detected_reference_hz) == pc::detected_reference_hz);
static_assert(std::string_view(pa::reference_offset_cents) == pc::reference_offset_cents);
static_assert(std::string_view(pa::reference_offset_warning) == pc::reference_offset_warning);
} // namespace tuning_fields_detail

/// The tuning fields every intonation response carries (pitch_accuracy and
/// pitch_curve): which grid the cents are measured on, the take's own
/// concert A, and whether the take sits > 10 cents off that grid.
struct TuningReading {
    double gridHz = 440.0;
    std::optional<double> detectedHz;
    double offsetCents = 0.0; ///< take's offset from A=440
    bool offsetWarning = false;

    static TuningReading fromResponse(const juce::var& v)
    {
        TuningReading t;
        const juce::var grid =
            v[api::fields::PitchAccuracyResponse::measurement_reference_hz].isVoid() ? v[api::fields::PitchAccuracyResponse::reference_hz] : v[api::fields::PitchAccuracyResponse::measurement_reference_hz];
        if (!grid.isVoid() && static_cast<double>(grid) > 0.0) t.gridHz = static_cast<double>(grid);
        if (!v[api::fields::PitchAccuracyResponse::detected_reference_hz].isVoid()) t.detectedHz = static_cast<double>(v[api::fields::PitchAccuracyResponse::detected_reference_hz]);
        if (!v[api::fields::PitchAccuracyResponse::reference_offset_cents].isVoid()) t.offsetCents = static_cast<double>(v[api::fields::PitchAccuracyResponse::reference_offset_cents]);
        t.offsetWarning = v[api::fields::PitchAccuracyResponse::reference_offset_warning].isBool() && static_cast<bool>(v[api::fields::PitchAccuracyResponse::reference_offset_warning]);
        return t;
    }

    /// Take offset from the measurement grid in cents, wrapped to +/-50.
    double offsetFromGridCents() const
    {
        if (!detectedHz.has_value() || *detectedHz <= 0.0 || gridHz <= 0.0) return 0.0;
        const double c = 1200.0 * std::log2(*detectedHz / gridHz);
        return c - 100.0 * std::floor((c + 50.0) / 100.0);
    }

    /// "Measured against A=432.0 Hz" - always shown so the grid is explicit.
    juce::String gridText() const { return TRANS("Measured against A=") + juce::String(gridHz, 1) + TRANS(" Hz"); }

    /// Caveat shown when the service flags the take as off the grid; empty otherwise.
    juce::String warningText() const
    {
        if (!offsetWarning) return {};
        const double off = offsetFromGridCents();
        return TRANS("Take tuning sits ") + (off >= 0.0 ? "+" : "") + juce::String(juce::roundToInt(off)) +
               TRANS(" cents off the grid (its own A=") +
               (detectedHz.has_value() ? juce::String(*detectedHz, 1) : juce::String("?")) +
               TRANS(" Hz) - check the reference before correcting.");
    }
};

} // namespace khensu
