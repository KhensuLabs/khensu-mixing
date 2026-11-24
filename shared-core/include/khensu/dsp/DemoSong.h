// shared-core/include/khensu/dsp/DemoSong.h
// VIZ-U2 / VIZ-F3: the demo song - a full-length (3:44) piece synthesised
// here, deterministically, so the app can open a rich sample session on
// first run and verify_all can screenshot a real-length track, without a
// third-party recording in the repository or the installer.
// 120 BPM in A minor (Am - F - C - G), stereo, with sections the analysis
// finds (intro, verse, pre-chorus, chorus, bridge, outro) and a few things
// for the findings to catch: a heavier low end in the choruses, bright
// sixteenth hats, and a last chorus pushed into true-peak overs.
// THREAD_CONTEXT(Any)
#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>

namespace khensu::demo_song {

inline constexpr double kBpm = 120.0;
inline constexpr int kBars = 112;
inline constexpr double kDurationSec = kBars * 4 * 60.0 / kBpm; ///< 224 s

struct Section {
    const char* label;
    int firstBar, bars;
};
/// The arrangement, in bars.
const std::vector<Section>& arrangement();

/// Renders the song into a stereo buffer at `sampleRate`.
juce::AudioBuffer<float> render(double sampleRate = 44100.0);

/// Writes it as a 24-bit WAV; false on failure.
bool write(const juce::File& file, double sampleRate = 44100.0);

} // namespace khensu::demo_song
