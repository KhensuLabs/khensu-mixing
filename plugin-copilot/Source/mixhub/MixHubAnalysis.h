// plugin-copilot/Source/mixhub/MixHubAnalysis.h
// Pure, thread-agnostic logic behind the cross-instance Mix view: the shared
// log-band grid, the per-track frame every Copilot instance publishes, role
// guessing, track-vs-track masking, and the summed whole-mix balance.
// Nothing here touches the audio thread, the hub's threads, or the UI, so it
// is unit-tested directly (tests/plugin/CopilotMixHubTest.cpp).
#pragma once

#include <JuceHeader.h>

#include <array>
#include <vector>

namespace khensu::mixhub {

/// 24 log-spaced bands, 25 Hz to 16 kHz (about 0.4 octave each). Every
/// instance uses the same grid, so band i of one track lines up with band i
/// of every other.
inline constexpr int kNumBands = 24;
inline constexpr float kLowestCentreHz = 25.0f;
inline constexpr float kHighestCentreHz = 16000.0f;
/// Band level of digital silence.
inline constexpr float kSilentDb = -120.0f;

float bandCentreHz(int band) noexcept;
float bandLowEdgeHz(int band) noexcept;  ///< geometric midpoint to the band below
float bandHighEdgeHz(int band) noexcept; ///< geometric midpoint to the band above

/// Mean-square band levels in dB (a full-scale sine reads -3.01 dB in its band).
using BandLevels = std::array<float, kNumBands>;

BandLevels silentBands() noexcept;

/// Folds a one-sided power spectrum into the band grid. `binPower[k]` is the
/// mean-square contribution of bin k (k = 0 .. numBins-1, spacing `binHz`), so
/// a band's level is 10*log10 of the sum of its bins. A band narrower than one
/// bin (the lowest bands at short FFT sizes) takes the nearest bin's power
/// scaled by its share of that bin's width, so its density stays right.
BandLevels bandLevelsFromPower(const float* binPower, int numBins, double binHz);

/// One instance's latest analysis, as published to the hub.
struct TrackFrame {
    juce::uint64 instanceId = 0;
    juce::String trackName; ///< from the host (updateTrackProperties); may be empty
    juce::String role;      ///< userRole when set, else guessRole()
    juce::String userRole;  ///< explicit user choice (PLG-18); empty = guessed
    BandLevels bandDb = silentBands();
    float shortTermLufs = -96.0f;
    float peakDb = -96.0f;
    float width = 0.0f;     ///< 0 = mono, 1 = uncorrelated (same metric as the meters)
    double updatedMs = 0.0; ///< when the last frame built from NEW audio was published

    bool hasSignal() const noexcept;
    float loudestBandDb() const noexcept;
};

/// "Kick", "Bass", "Vocal", ... from the host track name; when the name says
/// nothing, a coarse spectral description ("Low-end", "Mid", "Bright"). A track
/// named like a bus ("Master", "Mix Bus", "2-Bus") is "Mix bus": it already
/// contains every other track, so masking and the summed view skip it.
///
/// Names are matched on WORD boundaries (PLG-18): "hat" no longer matches
/// "chat", "ride" no longer matches "override", "sub" no longer matches
/// "subgroup", and "Bass Drum" is a kick, not a bass. A trailing plural "s"
/// is accepted ("Vocals", "Drums").
juce::String guessRole(const juce::String& trackName, const BandLevels& bands);

inline const char* const kMixBusRole = "Mix bus";

/// The roles a user can pick for an instance (the Mix tab's role menu).
const juce::StringArray& selectableRoles();

/// Marks an UNTITLED instance that is plainly the mix bus (PLG-18): a frame
/// without a host name or user role whose spectrum is at least as loud as
/// every other track in (nearly) every band they occupy, with two or more
/// other tracks carrying signal. A master instance on the stereo out used to
/// be counted as one more track and double-counted in the summed view.
void markImplicitMixBus(std::vector<TrackFrame>& frames);

/// The name shown for a track: its host name, else "Track <id>".
juce::String displayName(const TrackFrame& frame);

struct MaskingOptions {
    float maxLevelGapDb = 6.0f;  ///< both tracks within this many dB in the band
    float floorDb = -60.0f;      ///< ignore bands quieter than this in either track
    float significanceDb = 18.0f;///< ...or more than this below that track's loudest band
    float minFreqHz = 30.0f;
    float maxFreqHz = 8000.0f;
};

struct MaskingConflict {
    juce::uint64 instanceA = 0, instanceB = 0;
    juce::String nameA, nameB;
    int band = 0;               ///< strongest overlapping band
    int bandLo = 0, bandHi = 0; ///< contiguous overlapping range around it
    float freqHz = 0.0f;
    float levelADb = 0.0f, levelBDb = 0.0f;
    float score = 0.0f;         ///< louder and closer overlaps score higher

    /// "Kick vs Bass at 60 Hz (levels 1.5 dB apart, 45-80 Hz)".
    juce::String describe() const;
    bool involves(juce::uint64 id) const noexcept { return instanceA == id || instanceB == id; }
};

/// Pairwise masking between every two signal-carrying, non-bus tracks: for
/// each pair the strongest band where both are significant for that track
/// and within maxLevelGapDb of each other. Sorted by score, at most maxResults.
std::vector<MaskingConflict> findMaskingConflicts(const std::vector<TrackFrame>& frames,
                                                  const MaskingOptions& options = {}, size_t maxResults = 8);

/// Formats a frequency for display: "60 Hz", "250 Hz", "2.5 kHz".
juce::String formatHz(float hz);

struct MixSummary {
    BandLevels mixBandDb = silentBands(); ///< power sum of every contributing track
    float estimatedLufs = -96.0f; ///< power sum of short-term loudness (assumes uncorrelated tracks)
    int contributingTracks = 0;   ///< signal-carrying, non-bus tracks
    float lowShare = 0.0f, midShare = 0.0f, highShare = 0.0f; ///< energy below 250 Hz / 250 Hz-4 kHz / above
    struct Share {
        juce::uint64 instanceId = 0;
        juce::String name;
        float loudnessShare = 0.0f; ///< this track's share of the summed loudness power
    };
    std::vector<Share> shares; ///< loudest first
};

/// The whole-mix view built from every contributing instance. A summed view
/// of separate tracks is an estimate: correlated tracks (a doubled vocal) sum
/// louder than this power sum says.
MixSummary summarise(const std::vector<TrackFrame>& frames);

} // namespace khensu::mixhub
