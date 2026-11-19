// desktop-app/Source/services/IStems.h
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <optional>

namespace khensu {

struct AnalysisResult;
struct StemSeparationInfo;
namespace ipc {
struct StemSeparationResult;
}

/// Stem separation and the vocal-stem analysis (ENG-1). Message thread.
class IStems {
  public:
    virtual ~IStems() = default;

    /// <source dir>/stems_<id>/ for the open session.
    virtual juce::File getStemsDirectory() const = 0;
    /// "vocals" / "drums" / "bass" / "other"; a non-existent File if missing.
    virtual juce::File getStemFile(const juce::String& stemName) const = 0;
    /// All four stems exist AND are real separated stems (not HPSS, AID-07).
    virtual bool hasStemsAvailable() const = 0;
    /// The stems on disk are the approximate HPSS split.
    virtual bool hasApproximateStems() const = 0;
    virtual StemSeparationInfo stemSeparationInfo() const = 0;
    virtual bool isSeparatingStems() const noexcept = 0;
    virtual const std::optional<ipc::StemSeparationResult>& getLastSeparationResult() const noexcept = 0;
    virtual void runStemSeparation(const juce::String& method,
                                   std::function<void(bool success, juce::String message)> callback) = 0;

    // ---- Vocal-stem analysis ----
    virtual bool hasVocalAnalysis() const noexcept = 0;
    virtual const AnalysisResult& currentVocalAnalysis() const noexcept = 0;
    /// Analyses the vocals stem already on disk if nothing has this run.
    virtual void ensureVocalAnalysis() = 0;
    virtual void addVocalAnalysisListener(void* key, std::function<void()> cb) = 0;
    virtual void removeVocalAnalysisListener(void* key) = 0;

    // ---- The current vocal (WF-4) ----
    /// The edited vocal the vocal tools process and Stem Rebalance sums into
    /// the mix: the newest vocal render that stacked on it, or empty for the
    /// separated vocals stem (or when that edit's file is gone).
    virtual juce::String currentVocalStem() const = 0;
    /// Makes `path` the current vocal; empty goes back to the separated stem.
    /// Notifies the current-vocal listeners and autosaves.
    virtual void setCurrentVocalStem(const juce::String& path) = 0;
    virtual void addCurrentVocalListener(void* key, std::function<void()> cb) = 0;
    virtual void removeCurrentVocalListener(void* key) = 0;
};

} // namespace khensu
