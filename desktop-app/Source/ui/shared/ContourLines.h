// desktop-app/Source/ui/shared/ContourLines.h
// STU-12: iso-level lines over a grid (marching squares), for the 3D
// waterfall's 6 dB contours. The grid is rows x cols of values; a segment's
// ends are in grid coordinates (row, col as floats, interpolated linearly
// along the cell edge the level crosses). Saddle cells take the two
// segments that keep the cell's average on the high side.
// THREAD_CONTEXT(Any) - pure.
#pragma once
#include <cstddef>
#include <vector>

namespace khensu::contour {

struct GridPoint {
    float row = 0.0f, col = 0.0f;
};
struct Segment {
    GridPoint a, b;
};

/// The segments where `grid` crosses `level`. `grid[r][c]`; rows must all be
/// at least `cols` long (shorter rows are skipped).
inline std::vector<Segment> segments(const std::vector<std::vector<float>>& grid, float level)
{
    std::vector<Segment> out;
    if (grid.size() < 2) return out;
    const size_t cols = grid.front().size();
    if (cols < 2) return out;
    auto lerp = [level](float v0, float v1) {
        const float d = v1 - v0;
        return d != 0.0f ? (level - v0) / d : 0.5f;
    };
    for (size_t r = 0; r + 1 < grid.size(); ++r) {
        const auto& top = grid[r];
        const auto& bot = grid[r + 1];
        if (top.size() < cols || bot.size() < cols) continue;
        const auto rf = static_cast<float>(r);
        for (size_t c = 0; c + 1 < cols; ++c) {
            const float v00 = top[c], v01 = top[c + 1], v11 = bot[c + 1], v10 = bot[c];
            const int idx =
                (v00 >= level ? 8 : 0) | (v01 >= level ? 4 : 0) | (v11 >= level ? 2 : 0) | (v10 >= level ? 1 : 0);
            if (idx == 0 || idx == 15) continue;
            const auto cf = static_cast<float>(c);
            // Crossing points on the four edges.
            const GridPoint n{rf, cf + lerp(v00, v01)};        // top edge
            const GridPoint e{rf + lerp(v01, v11), cf + 1.0f}; // right edge
            const GridPoint s{rf + 1.0f, cf + lerp(v10, v11)}; // bottom edge
            const GridPoint w{rf + lerp(v00, v10), cf};        // left edge
            const bool centreHigh = (v00 + v01 + v11 + v10) * 0.25f >= level;
            switch (idx) {
            case 1:
            case 14:
                out.push_back({w, s});
                break;
            case 2:
            case 13:
                out.push_back({s, e});
                break;
            case 3:
            case 12:
                out.push_back({w, e});
                break;
            case 4:
            case 11:
                out.push_back({n, e});
                break;
            case 6:
            case 9:
                out.push_back({n, s});
                break;
            case 7:
            case 8:
                out.push_back({w, n});
                break;
            case 5: // v01 and v10 high
                if (centreHigh) {
                    out.push_back({w, n});
                    out.push_back({s, e});
                } else {
                    out.push_back({n, e});
                    out.push_back({w, s});
                }
                break;
            case 10: // v00 and v11 high
                if (centreHigh) {
                    out.push_back({n, e});
                    out.push_back({w, s});
                } else {
                    out.push_back({w, n});
                    out.push_back({s, e});
                }
                break;
            default:
                break;
            }
        }
    }
    return out;
}

} // namespace khensu::contour
