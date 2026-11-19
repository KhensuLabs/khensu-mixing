// shared-core/src/mixerchain/VocalMacroMapper.cpp
#include <khensu/mixerchain/VocalMacroMapper.h>
#include <cmath>

namespace khensu::mixerchain {

namespace {
using EffectSpec = khensu::ipc::HttpServiceBridge::EffectSpec;

double norm01(int value) { return juce::jlimit(0.0, 100.0, (double)value) / 100.0; }

void addIfNonZero(std::vector<EffectSpec>& chain, int macroValue, const char* effectName,
                  std::map<juce::String, float> params)
{
    if (macroValue <= 0) return;
    chain.push_back({juce::String(effectName), std::move(params)});
}
} // namespace

std::vector<EffectSpec> buildVocalMacroChain(const VocalMacroValues& v)
{
    std::vector<EffectSpec> chain;

    // 1. De-essing -> DeEsser. ratio mirrors the label's "Sens" formula;
    // threshold_db is derived from the label's "Range" (deeper de-essing at
    // higher slider values); frequency_hz is a fixed 6000 Hz -- the real
    // per-track dominant_sibilance_hz measurement lives in VocalAnalysisPanel's
    // own /analyze/vocal response, not in AppController's stored
    // AnalysisResult, so it isn't reachable here without a larger cross-panel
    // wiring change (see design doc's open-items note).
    {
        const double norm = norm01(v.deEssing);
        const float ratio = (float)(2.0 + std::pow(norm, 1.2) * 6.0); // 2..8
        const float rangeDb = (float)(std::pow(norm, 1.5) * 10.0);    // 0..10
        const float thresholdDb = -6.0f - rangeDb;                    // -6..-16
        addIfNonZero(chain, v.deEssing, "DeEsser",
                     {
                         {"threshold_db", thresholdDb},
                         {"ratio", ratio},
                         {"frequency_hz", 6000.0f},
                     });
    }

    // 2. Compression -> Compressor. threshold_db/ratio are identical to
    // VocalMacroPanel::updateLabelText's formula (idx 0). attack_ms/release_ms
    // are fixed musical vocal-compression defaults.
    {
        const double norm = norm01(v.compression);
        const float thresholdDb = (float)(-10.0 - std::pow(norm, 1.5) * 20.0); // -10..-30
        const float ratio = (float)(2.0 + std::pow(norm, 1.2) * 6.0);          // 2..8
        addIfNonZero(chain, v.compression, "Compressor",
                     {
                         {"threshold_db", thresholdDb},
                         {"ratio", ratio},
                         {"attack_ms", 15.0f},
                         {"release_ms", 120.0f},
                     });
    }

    // 3. Presence -> PeakFilter. gain_db is identical to the label's "HF
    // Boost" formula (idx 1). cutoff_frequency_hz/q are fixed to a classic
    // vocal presence band.
    {
        const double norm = norm01(v.presence);
        const float gainDb = (float)(0.5 + std::pow(norm, 1.5) * 4.5); // 0.5..5.0
        addIfNonZero(chain, v.presence, "PeakFilter",
                     {
                         {"cutoff_frequency_hz", 4000.0f},
                         {"gain_db", gainDb},
                         {"q", 1.0f},
                     });
    }

    // 4. Saturation -> TapeSaturation. drive_db is scaled from the label's
    // 0-50% "Drive" formula (idx 4) into 0-12 dB of TapeSaturation drive
    // (the effect accepts up to 24 dB; the macro deliberately uses half).
    // warmth tracks the slider directly; mix/output_trim_db are fixed.
    {
        const double norm = norm01(v.saturation);
        const float drivePercent = (float)(std::pow(norm, 1.5) * 50.0); // 0..50
        const float driveDb = drivePercent / 50.0f * 12.0f;             // 0..12 dB
        addIfNonZero(chain, v.saturation, "TapeSaturation",
                     {
                         {"drive_db", driveDb},
                         {"warmth", (float)norm},
                         {"mix", 0.5f},
                         {"output_trim_db", 0.0f},
                     });
    }

    // 5. Width -> StereoWidener. Directly derived from the label's "Width %"
    // formula (idx 2): 100%..200% maps onto StereoWidener's 1.0..2.0 unity-
    // to-double range.
    {
        const double norm = norm01(v.width);
        const float widthPercent = (float)(100.0 + std::pow(norm, 1.5) * 100.0); // 100..200
        addIfNonZero(chain, v.width, "StereoWidener",
                     {
                         {"width", widthPercent / 100.0f},
                     });
    }

    // 6. Reverb. wet_level is directly derived from the label's "Mix %"
    // formula (idx 5); room_size is derived from the label's "Decay"
    // formula, normalized into Reverb's 0..1 room_size range. damping and
    // dry_level are fixed.
    {
        const double norm = norm01(v.reverb);
        const float mixPercent = (float)(5.0 + std::pow(norm, 1.5) * 45.0); // 5..50
        const float decaySec = (float)(1.0 + std::pow(norm, 2.0) * 3.5);    // 1..4.5
        addIfNonZero(chain, v.reverb, "Reverb",
                     {
                         {"room_size", juce::jlimit(0.0f, 1.0f, decaySec / 5.0f)},
                         {"damping", 0.5f},
                         {"wet_level", mixPercent / 100.0f},
                         {"dry_level", 1.0f},
                     });
    }

    // 7. Delay. feedback/mix are directly derived from the label's formula
    // (idx 6); delay_seconds is a fixed dotted-eighth-ish default.
    {
        const double norm = norm01(v.delay);
        const float feedbackPercent = (float)(10.0 + std::pow(norm, 1.2) * 50.0); // 10..60
        const float mixPercent = (float)(5.0 + std::pow(norm, 1.5) * 35.0);       // 5..40
        addIfNonZero(chain, v.delay, "Delay",
                     {
                         {"delay_seconds", 0.25f},
                         {"feedback", feedbackPercent / 100.0f},
                         {"mix", mixPercent / 100.0f},
                     });
    }

    return chain;
}

} // namespace khensu::mixerchain
