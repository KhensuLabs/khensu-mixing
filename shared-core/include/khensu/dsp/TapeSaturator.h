#pragma once
// FN2-2a: the service's TapeSaturation (dsp_color._apply_tape_saturation) as
// a native chain stage, per channel:
//   up     = scipy.signal.resample_poly(x, 4, 1)   (81-tap Kaiser(5) FIR,
//            firwin(81, 1/4) x 4, zero padding, centred - 4n samples)
//   driven = up 10^(drive_db/20)
//   shaped = (1 - warmth) tanh(driven) + warmth (tanh(driven + 0.25) - tanh(0.25))
//   shaped = lfilter([1, -1], [1, -r], shaped), r = exp(-2 pi 5 / (4 sr))
//   down   = resample_poly(shaped, 1, 4)          (the same FIR x 1 - n samples)
//   out    = x (1 - mix) + down 10^(output_trim_db/20) mix
// Streamed: the FIRs look 40 oversampled samples ahead, so the stage holds
// back about 20 input samples and releases them in finish(), where the input
// past the end (and the oversampled signal past 4n) is zero, as resample_poly
// pads it.
// THREAD_CONTEXT(WorkerThread) - one instance per render.
#include <khensu/dsp/ChainProcessor.h>

#include <memory>
#include <vector>

namespace khensu::dsp {

/// scipy.signal.firwin(numTaps, cutoff, window=("kaiser", beta)): a linear-
/// phase low-pass, `cutoff` relative to Nyquist, scaled to unity gain at DC.
std::vector<double> firwinKaiser(int numTaps, double cutoff, double beta);

/// The TapeSaturation stage (parameters already clamped as the service clamps them).
std::unique_ptr<ChainStage> makeTapeSaturationStage(double driveDb, double warmth, double mix, double outputTrimDb,
                                                    double sampleRate, int numChannels);

} // namespace khensu::dsp
