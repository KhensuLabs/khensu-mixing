#pragma once
// OVH-9: render a file through an effect chain without the Python service.
//
// The service's /copilot/apply_chain job (copilot/dsp.py apply_pedalboard_chain
// + router_render's post-render guardrail) stays the reference whenever it is
// up; this is the same render for the chains every one-click fix builds
// (filters, shelves, peaks, compressor, de-esser, gain, true-peak limiter,
// and since FN2-2a spectral denoise, transient shaper, tape saturation and
// stereo widener - see ChainProcessor.h), so analyse -> fix -> fix check ->
// A/B keeps working while the engine is down or still starting.
//
// Two passes over the file, block by block (bounded memory on any length):
//   0. (only for a chain with a whole-signal stage - SpectralDenoise) the
//      file is decoded once more per such stage to build its profile
//      (ChainProcessor::runProfilePasses);
//   1. decode -> chain -> 32-bit float temp file, measuring the input and the
//      output (BS.1770 loudness, 4x true peak, 8x true peak after a final
//      Limiter, low-band L/R correlation);
//   2. the service's output stage as one gain: the final Limiter's residual
//      trim (tp_limiter: re-measure at 8x, scale under the ceiling) and the
//      clip guard (dsp._apply_output_gain: attenuate, never boost, when the
//      4x true peak is over the chain's ceiling, -1 dBTP by default) -
//      applied while copying to the output, or a rename when it is 0 dB.
// Then the guardrail's absolute checks (render_guardrails.post_render_guardrail):
// true peak over -1 dBTP and higher than the input's, and a mono low-band
// correlation drop of more than 0.1, refuse the render (the service would
// quarantine it; there is no override without it). matched_gain_db is the
// input-minus-output integrated loudness, as the service reports it.
// THREAD_CONTEXT(WorkerThread) - blocking file I/O and DSP.
#include <khensu/dsp/ChainProcessor.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace juce {
class File;
}

namespace khensu::dsp {

struct RenderLevels {
    double integratedLufs = std::numeric_limits<double>::quiet_NaN(); ///< NaN when unmeasurable
    double truePeakDbtp = -240.0;                                      ///< 4x, the meters' filter
    double monoLowCorrelation = 1.0;                                   ///< L/R below 120 Hz (1 for mono)
};

struct OfflineRenderOptions {
    int blockSize = 16384;
    /// Polled once per block; true stops the render (temp files removed).
    std::function<bool()> shouldCancel;
    /// 0..1, from the rendering thread.
    std::function<void(double)> onProgress;
    /// False keeps a render the guardrail would refuse (blocks are still
    /// reported): the parity tests compare the processing, not the gate.
    bool enforceGuardrail = true;
};

struct OfflineRenderResult {
    enum class Status { Rendered, Cancelled, Failed, NeedsEngine };
    Status status = Status::Failed;
    std::string error;                    ///< why, for Failed / NeedsEngine
    std::vector<std::string> applied;     ///< effect names, in order
    std::vector<std::string> needsEngine; ///< effects only the service renders
    double sampleRate = 0.0;
    int numChannels = 0;
    std::int64_t numSamples = 0;
    double outputGainDb = 0.0; ///< the output stage (limiter trim + clip guard)
    RenderLevels before, after;
    double matchedGainDb = 0.0; ///< before - after integrated loudness, 0 when unmeasurable
    bool guardrailOk = true;
    std::vector<std::string> blocks;   ///< why the guardrail refused it
    std::vector<std::string> warnings; ///< softer notes
};

class OfflineChainRenderer {
  public:
    /// Whether `chain` can render natively for a file with `numChannels`
    /// (mono or stereo; surround routing is the service's): empty chains and
    /// chains with an engine-only effect cannot.
    static bool canRender(const std::vector<ChainStageSpec>& chain, int numChannels = 2);

    /// Renders `input` through `chain` into `output` (32-bit float WAV,
    /// replaced atomically; never written when the result is not Rendered).
    static OfflineRenderResult renderFile(const juce::File& input, const juce::File& output,
                                          const std::vector<ChainStageSpec>& chain,
                                          const OfflineRenderOptions& options = {});
};

} // namespace khensu::dsp
