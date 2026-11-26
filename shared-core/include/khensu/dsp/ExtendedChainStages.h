#pragma once
// ENH-6: the chain stages the native renderer was missing, each matched to
// the service's processor of the same name and parameters (copilot/dsp*.py,
// clamps in dsp_params._PARAM_CLAMPS) so one chain JSON renders the same on
// both sides:
//
//   DynamicEQ            dsp_dynamics._apply_dynamic_eq: a zero-phase
//                        4th-order Butterworth band (sosfiltfilt) around
//                        frequency_hz (1/q octaves wide), a linked
//                        attack/release follower of its mean |band|, and
//                        out = (x - band) + band * gr, gr = (env/threshold)^
//                        (1/ratio - 1) over the threshold, floored at
//                        -max_reduction_db. Native extensions (the service
//                        drops unknown keys, so their defaults are its
//                        behaviour): `shape` 0 bell / 1 low shelf (band =
//                        zero-phase low-pass at frequency_hz) / 2 high shelf,
//                        and `use_key` 1 = detect on the render's KEY signal
//                        (a stem: the vocal keying a cut in the bed) through
//                        the same band filter instead of the input itself.
//   MultibandCompressor  dsp_dynamics._apply_multiband_compressor: three
//                        zero-phase Butterworth bands (_band_split: 4th order
//                        run forward and back = a Linkwitz-Riley 8 magnitude,
//                        no phase shift), each through the glue compressor's
//                        core (linked peak detector, 6 dB soft knee), summed,
//                        times makeup. Native extension: optional per-band
//                        low_/mid_/high_ attack_ms, release_ms and makeup_db
//                        (default: the shared ones).
//   MidSideProcessor     dsp_spatial._apply_mid_side_processor: mid and side
//                        gains. Native extension: an RBJ bell on each of mid
//                        and side (mid_eq_* / side_eq_*: frequency_hz,
//                        gain_db, q) and a 12 dB/oct side high-pass
//                        (side_highpass_frequency_hz; 0 = off).
//   Exciter              dsp_color._apply_exciter: zero-phase high-pass band,
//                        tanh(drive (b + even b^2)), high-passed again, scaled
//                        to the band's whole-signal RMS (a profile pass) and
//                        mixed in.
//   BassMonoMaker        dsp_spatial._apply_bass_mono_maker: zero-phase
//                        low band of the mid, plus each side's zero-phase
//                        high band.
//   NoiseGate            pedalboard.NoiseGate (juce::dsp::NoiseGate): 50 ms
//                        RMS ballistics, then attack/release ballistics, gain
//                        (env/threshold)^(ratio - 1) below the threshold.
//
// The zero-phase stages run their filters over chunks with enough real
// context either side for the filters to ring out (from the slowest pole),
// so they hold back a chunk plus that margin and release it in finish().
// Every buffer is sized when the stage is built; once its history has grown
// to the block size it is fed, push() does not allocate (the block it hands
// back is the caller's).
// THREAD_CONTEXT(WorkerThread) - one instance per render.
#include <khensu/dsp/ChainProcessor.h>
#include <khensu/dsp/IirDesign.h>

#include <map>
#include <memory>
#include <string>

namespace khensu::dsp {

/// `p` holds every rule of the effect (ChainProcessor's resolved, clamped
/// parameters); optional native-only keys may be absent.
std::unique_ptr<ChainStage> makeDynamicEqStage(const std::map<std::string, double>& p, double sampleRate,
                                               int numChannels);
std::unique_ptr<ChainStage> makeMultibandCompressorStage(const std::map<std::string, double>& p, double sampleRate,
                                                         int numChannels);
std::unique_ptr<ChainStage> makeMidSideStage(const std::map<std::string, double>& p, double sampleRate,
                                             int numChannels);
std::unique_ptr<ChainStage> makeExciterStage(const std::map<std::string, double>& p, double sampleRate,
                                             int numChannels);
std::unique_ptr<ChainStage> makeBassMonoMakerStage(const std::map<std::string, double>& p, double sampleRate,
                                                   int numChannels);
std::unique_ptr<ChainStage> makeNoiseGateStage(const std::map<std::string, double>& p, double sampleRate,
                                               int numChannels);

/// Samples a biquad cascade needs to ring down by 140 dB from its slowest
/// pole (x 1.5 for a resonant section's gain) - the context a chunked
/// zero-phase pass keeps either side.
int zeroPhaseSettleSamples(const std::vector<BiquadCoefficients>& sections);

} // namespace khensu::dsp
