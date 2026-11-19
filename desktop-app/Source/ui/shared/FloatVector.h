// desktop-app/Source/ui/shared/FloatVector.h
#pragma once
#include <vector>

namespace khensu {

/// Copies a range of doubles (a service payload) into the floats a panel
/// draws with. The narrowing is deliberate, so it is spelled out (SHIP-6:
/// C4244 is on for project code).
template <typename Range> std::vector<float> toFloats(const Range& values)
{
    std::vector<float> out;
    out.reserve(values.size());
    for (const auto v : values) out.push_back(static_cast<float>(v));
    return out;
}

} // namespace khensu
