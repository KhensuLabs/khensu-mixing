// desktop-app/Source/session/MultitrackStemBuses.h
// WF-7: a multitrack session's own stems in the four stem slots. The stem
// tools (vocal workspace, masking, Stem Rebalance, the AI deep tier) read
// <stems dir>/{vocals,drums,bass,other}.wav; for a session imported from a
// folder of bounced tracks those come from the tracks themselves - each
// slot the sum of the tracks whose role maps to it - instead of Demucs
// guessing them back out of the mixdown.
// THREAD_CONTEXT(WorkerThread) - file reads and writes.
#pragma once
#include <JuceHeader.h>
#include <khensu/session/AudioFileRef.h>
#include <map>
#include <vector>

namespace khensu::multitrack_stems {

/// The written stems' separation.json method: real stems, not a guess.
inline constexpr const char* kMethod = "multitrack";

/// The stem slot a track role sums into: "vocals", "drums" (kick, snare,
/// percussion, drums), "bass", or "other" (synth, FX, unknown roles). The
/// summed mixdown ("mix") goes nowhere - it is the mix itself.
juce::String slotForRole(const juce::String& role);

/// Slot -> the tracks summed into it (the mixdown excluded).
std::map<juce::String, juce::StringArray> groupBySlot(const std::vector<AudioFileRef>& files);

struct Outcome {
    bool ok = false;
    juce::String message;    ///< what was written, or why not
    juce::StringArray empty; ///< slots with no track (written as silence so all four exist)
};

/// Sums each slot's tracks at unity gain into <dir>/<slot>.wav (32-bit float,
/// stereo, the first track's sample rate) and writes separation.json with
/// method "multitrack". A track at another sample rate is left out (and
/// named in the message). A slot without tracks is written silent. Leaves
/// existing files untouched unless `overwrite`.
Outcome writeStemBuses(const std::vector<AudioFileRef>& files, const juce::File& dir, bool overwrite = false);

} // namespace khensu::multitrack_stems
