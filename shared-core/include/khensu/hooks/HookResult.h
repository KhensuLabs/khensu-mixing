#pragma once
#include <khensu/hooks/HookType.h>
#include <vector>

namespace khensu {

struct HookInstance {
    float start_sec = 0.0f;
    float end_sec = 0.0f;
    HookType type = HookType::Structural;
    float strength = 0.0f;
};

struct HookResult {
    std::vector<HookInstance> instances;
    float hook_strength = 0.0f;
    float hook_density = 0.0f;
    int hook_count = 0;
};

} // namespace khensu
