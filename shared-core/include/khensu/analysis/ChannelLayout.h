#pragma once
#include <khensu/common/Result.h>
#include <array>
#include <string>

namespace khensu {

/// Role of a single channel within a supported layout, in SMPTE/WAV order.
enum class ChannelRole { Left, Right, Centre, LFE, LeftSurround, RightSurround, LeftBack, RightBack };

/// The layouts this product accepts. Unspecified is a backward-compatibility
/// value only - see ChannelLayout::unspecified().
enum class LayoutId { Mono, Stereo, Surround51, Surround71, Unspecified };

/// Describes a supported channel layout: which role each channel carries, and
/// how ITU-R BS.1770 treats it when summing loudness.
///
/// This is the single place layout policy lives. Analyzers consume the
/// descriptor rather than re-deriving policy from a raw channel count.
///
/// Layout is inferred from the channel count: JUCE's AudioFormatReader does not
/// surface a WAV channel mask through the API used here, so a 6-channel file is
/// treated as 5.1 in standard order. That is a documented assumption, not
/// invented precision.
///
/// THREAD_CONTEXT(AnyThread) - value type, safe everywhere once constructed.
struct ChannelLayout {
    static constexpr int kMaxChannels = 8;

    /// BS.1770 surround weight, +1.5 dB expressed in the POWER domain
    /// (10^(1.5/10)). It multiplies the squared K-weighted sample, never the
    /// amplitude - applying it to an amplitude would double the intended boost.
    static constexpr double kSurroundWeight = 1.4125375446227544;

    LayoutId id = LayoutId::Stereo;
    int numChannels = 2;
    const char* name = "Stereo";
    std::array<ChannelRole, kMaxChannels> roles{};

    /// Resolve a layout from a channel count. Accepts 1, 2, 6, 8; every other
    /// count is an error naming the count and the supported set.
    static Result<ChannelLayout> fromChannelCount(int channelCount);

    /// An all-unity, no-LFE layout that reproduces the pre-contract behaviour.
    /// Used only by the backward-compatible count-based reset() overloads, for
    /// counts AudioAnalyzer rejects before any analyzer is constructed.
    static ChannelLayout unspecified(int channelCount);

    /// Power-domain BS.1770 weight: 1.0 for L/R/C, 0.0 for LFE (excluded),
    /// kSurroundWeight for the surround/back channels. Out-of-range indices
    /// return 1.0 so a caller can never accidentally zero real audio.
    double bs1770Weight(int channelIndex) const noexcept;

    bool isLFE(int channelIndex) const noexcept;
    bool hasLFE() const noexcept;

    int frontLeft() const noexcept { return 0; }
    /// Mono folds the right channel onto the left.
    int frontRight() const noexcept { return numChannels > 1 ? 1 : 0; }
};

} // namespace khensu
