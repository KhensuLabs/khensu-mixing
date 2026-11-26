// desktop-app/Source/services/EqBandChain.h
// The EQ editor's bands as a render chain - one place, so what Apply EQ
// renders (RenderService::applyEqBands) and what the live preview plays
// (STU-9) are the same chain.
#pragma once
#include <cmath>
#include <vector>
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ui/EqCurveComponent.h>

namespace khensu {

/// Enabled bands only; a band whose shape has a gain is left out under
/// 0.1 dB (HPF/LPF have no gain: they always render when enabled).
inline std::vector<ipc::HttpServiceBridge::EffectSpec> eqBandsToChain(const std::vector<EqBand>& bands)
{
    std::vector<ipc::HttpServiceBridge::EffectSpec> chain;
    for (const auto& band : bands) {
        if (!band.enabled || (eqBandTypeHasGain(band.type) && std::abs(band.gain_db) < 0.1f)) continue;
        ipc::HttpServiceBridge::EffectSpec eq;
        eq.effect = renderEffectName(band.type); // AUDIT-008: shelves render as shelves, not bells
        eq.parameters["cutoff_frequency_hz"] = band.hz;
        eq.parameters["gain_db"] = band.gain_db;
        eq.parameters["q"] = band.q;
        chain.push_back(std::move(eq));
    }
    return chain;
}

} // namespace khensu
