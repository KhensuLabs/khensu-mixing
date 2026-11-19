#pragma once
#include <khensu/ipc/BridgeTransport.h>

namespace khensu::ipc {

/// Performance / timing / space analyses (router_performance.py) (ENG-4). Implemented in
/// src/ipc/HttpServiceBridge_Performance.cpp. Usable on its own (`PerformanceApi{}.method(...)`)
/// or through HttpServiceBridge, which derives from every domain client.
class PerformanceApi : public virtual BridgeTransport {
  public:
    PerformanceApi() = default;
    explicit PerformanceApi(std::string base_url, int timeout_ms = kDefaultTimeoutMs,
                            int retry_delay_ms = kDefaultRetryDelayMs)
        : BridgeTransport(std::move(base_url), timeout_ms, retry_delay_ms)
    {}

    /// One platform codec the codec audit can encode through
    /// (codec_artifacts.PLATFORM_CODECS).
    struct CodecPlatformInfo {
        juce::String id;    ///< what /analyze/codec/audit's `codecs` takes
        juce::String name;  ///< display name, e.g. "Spotify (AAC 256)"
        juce::String codec; ///< ffmpeg codec family ("aac", "libopus", ...)
        int bitrateK = 0;   ///< kbit/s; 0 = unknown
    };
    /// GET /analyze/codec/platforms - the live platform table (WIRE §9.2).
    khensu::Result<std::vector<CodecPlatformInfo>, BridgeError> getCodecPlatforms();
    /// Parses a CodecPlatformsResponse body; rows without an id are dropped,
    /// a missing name falls back to the id.
    static std::vector<CodecPlatformInfo> parseCodecPlatforms(const juce::var& body);

    // ---- Performance / timing / space analyses (router_performance.py) ----
    // Typed wrappers for the four routes that had no UI (audit §9.2). Each
    // decodes the whole file server-side; sized timeouts, not the 30 s default.

    /// POST /analyze/groove_timing - where onsets sit against the tracked
    /// beat grid (8th / 16th / triplet subdivision chosen from the onsets).
    struct GrooveTimingResult {
        double bpm = 0.0;
        double gridOffsetMs = 0.0;    ///< median onset offset; < 0 = ahead (pushing)
        double timingStdMs = 0.0;     ///< spread (std dev) of the offsets
        double aheadFraction = 0.0;   ///< share of onsets > 12 ms early, 0..1
        double behindFraction = 0.0;  ///< share of onsets > 12 ms late, 0..1
        double swingRatio = 1.0;      ///< long:short off-beat ratio; 1 = straight, 2 = triplet
        juce::String pocket;          ///< on_grid / ahead_of_grid / behind_grid / insufficient_onsets
        juce::String gridSubdivision; ///< 8th / 16th / triplet / none
        struct Event {
            double timeSec = 0.0;
            double offsetMs = 0.0;
        };
        std::vector<Event> events; ///< first <= 160 onsets
    };
    khensu::Result<GrooveTimingResult, BridgeError> analyzeGrooveTiming(const juce::String& filePath);
    static GrooveTimingResult parseGrooveTiming(const juce::var& body);

    /// POST /analyze/harmony_timeline - chord progression (repeats merged,
    /// "N" left out) and sustained key changes over time.
    struct HarmonyTimelineResult {
        struct Chord {
            double timeSec = 0.0;
            juce::String chord;
            double confidence = 0.0;
        };
        struct KeyPoint {
            double timeSec = 0.0;
            juce::String key;  ///< tonic, e.g. "F#"
            juce::String mode; ///< "major" / "minor"
        };
        std::optional<KeyPoint> initialKey;
        std::vector<KeyPoint> keyChanges;
        std::vector<Chord> progression;
    };
    khensu::Result<HarmonyTimelineResult, BridgeError> analyzeHarmonyTimeline(const juce::String& filePath);
    static HarmonyTimelineResult parseHarmonyTimeline(const juce::var& body);

    /// POST /analyze/reverb_tail - free-decay RT60 (median over stops /
    /// endings / isolated hits) and the onset-relative tail energy ratio.
    struct ReverbTailResult {
        std::optional<double> rt60Sec; ///< absent when no usable free decay exists
        double fitR2 = 0.0;
        double tailEnergyRatio = 0.0; ///< between-onset / onset energy, 0..1+
        int decaySegments = 0;
        juce::String tailLabel; ///< pronounced / moderate / dry_or_short / insufficient_audio
        juce::String note;      ///< the service's explanation of the RT60
    };
    khensu::Result<ReverbTailResult, BridgeError> analyzeReverbTail(const juce::String& filePath);
    static ReverbTailResult parseReverbTail(const juce::var& body);

    /// POST /analyze/stem_phase - pairwise broadband correlation + polarity
    /// of 2..16 stems (name -> path).
    struct StemPhaseResult {
        struct Pair {
            juce::String left, right;
            double correlation = 0.0; ///< -1..1
            juce::String polarity;    ///< aligned / inverted / decorrelated
        };
        std::vector<Pair> pairs;
        int stemCount = 0;
    };
    khensu::Result<StemPhaseResult, BridgeError>
    analyzeStemPhase(const std::map<juce::String, juce::String>& stemPaths);
    static StemPhaseResult parseStemPhase(const juce::var& body);

    /// POST /analyze/vocal_harmony - writes one diatonic harmony line (a 3rd
    /// or 5th above/below the extracted vocal) as a standard MIDI file.
    struct VocalHarmonyOptions {
        juce::String interval = "third";  ///< "third" / "fifth"
        juce::String direction = "above"; ///< "above" / "below"
        juce::String key;                 ///< "" = detect from the audio
        juce::String mode;                ///< "" / "major" / "minor" (needs a key)
        double bpm = 0.0;                 ///< 0 = detect
        juce::String outputPath;          ///< "" = service default next to the source
    };
    struct VocalHarmonyResult {
        juce::String outputPath;
        juce::String key, mode, keySource;
        double keyConfidence = 0.0;
        double bpm = 0.0;
        juce::String bpmSource;
        juce::String interval, direction;
        juce::String timeSignature;
        juce::String extractionBackend;
        bool lowQualityExtraction = false;
        int eventCount = 0;
        struct Event {
            double startSec = 0.0, endSec = 0.0;
            juce::String sourceNote, harmonyNote;
        };
        std::vector<Event> events;
    };
    khensu::Result<VocalHarmonyResult, BridgeError> generateVocalHarmony(const juce::String& filePath,
                                                                         const VocalHarmonyOptions& options);
    static VocalHarmonyResult parseVocalHarmony(const juce::var& body);

    /// POST /analyze/fingerprint_compare - local chroma-hash comparison of two
    /// files ("same recording?"). Nothing leaves the machine.
    struct FingerprintCompareResult {
        double similarity = 0.0; ///< 0..1
        juce::String verdict;    ///< same_recording / same_song_different_version / different
        juce::String method;
        int framesA = 0, framesB = 0;
    };
    khensu::Result<FingerprintCompareResult, BridgeError> compareFingerprints(const juce::String& pathA,
                                                                              const juce::String& pathB);
    static FingerprintCompareResult parseFingerprintCompare(const juce::var& body);
};

} // namespace khensu::ipc
