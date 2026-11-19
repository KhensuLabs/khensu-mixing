// Channel-layout policy: role order, BS.1770 weighting, LFE identification.
// THREAD_CONTEXT(AnyThread)
#include <khensu/analysis/ChannelLayout.h>

#include <initializer_list>

namespace khensu {

namespace {

ChannelLayout makeLayout(LayoutId id, int numChannels, const char* name,
                         std::initializer_list<ChannelRole> roles)
{
    ChannelLayout layout;
    layout.id = id;
    layout.numChannels = numChannels;
    layout.name = name;
    int i = 0;
    for (ChannelRole r : roles) {
        if (i >= ChannelLayout::kMaxChannels) break;
        layout.roles[static_cast<size_t>(i++)] = r;
    }
    return layout;
}

} // namespace

Result<ChannelLayout> ChannelLayout::fromChannelCount(int channelCount)
{
    using R = ChannelRole;
    switch (channelCount) {
    case 1:
        return Result<ChannelLayout>::ok(makeLayout(LayoutId::Mono, 1, "Mono", {R::Centre}));
    case 2:
        return Result<ChannelLayout>::ok(makeLayout(LayoutId::Stereo, 2, "Stereo", {R::Left, R::Right}));
    case 6:
        return Result<ChannelLayout>::ok(makeLayout(
            LayoutId::Surround51, 6, "5.1",
            {R::Left, R::Right, R::Centre, R::LFE, R::LeftSurround, R::RightSurround}));
    case 8:
        return Result<ChannelLayout>::ok(makeLayout(
            LayoutId::Surround71, 8, "7.1",
            {R::Left, R::Right, R::Centre, R::LFE, R::LeftSurround, R::RightSurround, R::LeftBack,
             R::RightBack}));
    default:
        return Result<ChannelLayout>::err(
            "Unsupported channel layout: " + std::to_string(channelCount) +
            " channels. Supported layouts are 1 (Mono), 2 (Stereo), 6 (5.1), and 8 (7.1).");
    }
}

ChannelLayout ChannelLayout::unspecified(int channelCount)
{
    ChannelLayout layout;
    layout.id = LayoutId::Unspecified;
    layout.numChannels = channelCount;
    layout.name = "Unspecified";
    // Roles stay default-initialised; bs1770Weight()/isLFE() short-circuit on
    // LayoutId::Unspecified, so no role is ever consulted.
    return layout;
}

double ChannelLayout::bs1770Weight(int channelIndex) const noexcept
{
    if (id == LayoutId::Unspecified) return 1.0;
    if (channelIndex < 0 || channelIndex >= numChannels || channelIndex >= kMaxChannels) return 1.0;

    switch (roles[static_cast<size_t>(channelIndex)]) {
    case ChannelRole::LFE:
        return 0.0;
    case ChannelRole::LeftSurround:
    case ChannelRole::RightSurround:
    case ChannelRole::LeftBack:
    case ChannelRole::RightBack:
        return kSurroundWeight;
    case ChannelRole::Left:
    case ChannelRole::Right:
    case ChannelRole::Centre:
    default:
        return 1.0;
    }
}

bool ChannelLayout::isLFE(int channelIndex) const noexcept
{
    if (id == LayoutId::Unspecified) return false;
    if (channelIndex < 0 || channelIndex >= numChannels || channelIndex >= kMaxChannels) return false;
    return roles[static_cast<size_t>(channelIndex)] == ChannelRole::LFE;
}

bool ChannelLayout::hasLFE() const noexcept
{
    for (int c = 0; c < numChannels && c < kMaxChannels; ++c)
        if (isLFE(c)) return true;
    return false;
}

} // namespace khensu
