// shared-core/include/khensu/mixerchain/VocalMacroMapper.h
#pragma once
#include <khensu/ipc/HttpServiceBridge.h>
#include <vector>

namespace khensu::mixerchain {

/// Plain 0-100 values from the 7 Vocal Macro sliders (VocalMacroPanel).
/// Field order matches VocalMacroPanel::kMacroNames / the macro strategy
/// preset arrays: Compression, Presence, Width, De-essing, Saturation,
/// Reverb, Delay.
struct VocalMacroValues {
    int compression = 0;
    int presence = 0;
    int width = 0;
    int deEssing = 0;
    int saturation = 0;
    int reverb = 0;
    int delay = 0;
};

/// Turns 7 macro slider values into a real, renderable effect chain for the
/// isolated vocal stem. Reuses the exact formulas VocalMacroPanel::updateLabelText
/// already computes for its on-screen labels (Compressor threshold/ratio, EQ
/// boost, stereo width, saturation drive, reverb decay/mix, delay
/// feedback/mix) so the rendered audio always matches what the label
/// promises. Fields with no existing label formula (DeEsser threshold_db,
/// all attack/release times, PeakFilter cutoff/Q, Reverb room_size/damping,
/// Delay delay_seconds, TapeSaturation mix/output_trim_db) use fixed,
/// musically-reasonable constants documented inline in the .cpp.
///
/// A macro at value 0 is omitted from the chain entirely - never inserted
/// as a no-op effect - so ApplyChainResponse::applied_effects stays honest
/// about what actually ran.
///
/// Signal order (dynamics/tone before space/width, standard vocal chain
/// convention): DeEsser -> Compressor -> PeakFilter -> TapeSaturation ->
/// StereoWidener -> Reverb -> Delay.
///
/// Effect names/parameter ranges must stay in sync with
/// local-services/khensu_services/copilot/dsp.py's _PARAM_CLAMPS table -
/// there is no compile-time link between the two, this is a manually-kept
/// invariant (verified by test_vocal_macro_mapper.cpp's range-check test).
std::vector<khensu::ipc::HttpServiceBridge::EffectSpec> buildVocalMacroChain(const VocalMacroValues& v);

} // namespace khensu::mixerchain
