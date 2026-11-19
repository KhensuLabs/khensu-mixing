// desktop-app/Source/ui/SessionIntake/MultitrackModel.h
// Typed view of the /analyze/multitrack/* responses (a session imported from
// a folder of bounced stems). Parsing is strict on the fields the views
// depend on: a missing or renamed required key is a parse error naming the
// key, never a track that silently shows 0 LUFS.
#pragma once
#include <JuceHeader.h>
#include <khensu/common/Result.h>
#include <khensu/session/AudioFileRef.h>
#include <optional>
#include <string>
#include <vector>

namespace khensu::multitrack {

struct Mismatch {
    juce::String track, path, kind, detail; // kind: sample_rate | length | channels
};

struct ScanFile {
    juce::String path, fileName, trackName, role, roleLabel, roleSource;
    int sampleRate = 0;
    int channels = 0;
    double durationSec = 0.0;
};

struct Scan {
    juce::String folder;
    int sessionSampleRate = 0;
    bool consistent = true;
    int maxTracks = 0;
    std::vector<ScanFile> files;
    std::vector<Mismatch> mismatches;
};

struct Advice {
    juce::String issueCode, severity, what, suggestedAction, whyHelps, relatedTrack;
};

struct Track {
    juce::String name, path, fileName, role, roleLabel, roleSource;
    double roleConfidence = 0.0;
    int sampleRate = 0;
    int channels = 0;
    double durationSec = 0.0;
    std::optional<double> integratedLufs, truePeakDbtp, samplePeakDbfs, crestDb, correlation;
    double stereoWidth = 0.0;
    double lowEndWidth = 0.0;
    bool silent = false;
    std::vector<Advice> advice;

    /// Count of advice items at `severity` ("severe" / "moderate" / "minor").
    int adviceCount(const juce::String& severity) const;
};

struct Pair {
    juce::String trackA, trackB, bandLabel, riskLevel, summary; // riskLevel: high | moderate | low
    double collisionScore = 0.0, bandOverlap = 0.0, levelDiffDb = 0.0, risk = 0.0;
    double bandLoHz = 0.0, bandHiHz = 0.0;
};

struct Mix {
    int sampleRate = 0;
    std::optional<double> integratedLufs, truePeakDbtp, stereoWidth, correlation, durationSec;
    bool headroomOk = true;
    juce::String mixdownPath; // empty when no mixdown was written
};

struct Analysis {
    int sessionSampleRate = 0;
    bool consistent = true;
    std::vector<Mismatch> mismatches;
    std::vector<Track> tracks;
    std::vector<Pair> pairs; // highest risk first
    std::vector<juce::String> matrixTracks;
    std::vector<std::vector<double>> matrix; // symmetric risk 0..1, matrixTracks order

    /// Risk between two tracks by name (0 when either is unknown).
    double riskBetween(const juce::String& a, const juce::String& b) const;
    Mix mix;
};

struct JobStatus {
    juce::String status; // see khensu::api::JobStatus
    int total = 0;
    int done = 0;
    juce::String current;
    juce::String error;
    std::optional<Analysis> result;

    bool isTerminal() const;
    bool isCompleted() const;
};

khensu::Result<Scan, std::string> parseScan(const juce::var& v);
khensu::Result<Analysis, std::string> parseAnalysis(const juce::var& v);
khensu::Result<JobStatus, std::string> parseJobStatus(const juce::var& v);

/// Session files for a finished analysis: the summed mixdown first (role
/// "mix", so the dashboard analyses and plays the whole song), then every
/// stem with its track name and role. Without a mixdown the first stem
/// leads. Ids are fresh UUIDs.
std::vector<AudioFileRef> buildSessionFiles(const Analysis& analysis);

/// Masking risk level -> the shared severity scale used for colour
/// ("high" -> "severe", "moderate" -> "moderate", anything else -> "minor").
juce::String severityForRiskLevel(const juce::String& riskLevel);

} // namespace khensu::multitrack
