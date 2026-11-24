// desktop-app/Source/services/DeepPassState.h
// OVH-1: why the current result has (or lacks) its deep half, so every
// surface words it by the real reason (ui/shared/ServiceStatusText.h) instead
// of "the analysis engine was unreachable" for all of them.
#pragma once

namespace khensu {

enum class DeepPassState {
    Complete,          ///< the deep pass ran and answered
    Off,               ///< deep analysis is switched off for this session
    SkippedByUser,     ///< the user chose "fast results only"
    Failed,            ///< the engine answered, but the deep job failed
    EngineUnavailable, ///< the engine was not running (or did not answer)
};

} // namespace khensu
