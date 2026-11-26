// desktop-app/Source/session/PlaybackSourceController.h
#pragma once
#include <JuceHeader.h>
#include <optional>
#include "../services/AbLevelOffset.h"

namespace khensu {

/// A/B audition state + loudness-matched playback gain, split out of
/// AppController (audit refactor "Split AppController"). Holds which render is
/// the "B" side, whether B is showing, B's measured loudness, and the user /
/// streaming-preview gain offsets. gainFor() is pure, so the A/B matching rules
/// are unit-testable without an audio device.
class PlaybackSourceController final {
  public:
    juce::String processedPath;            ///< the B-side render ("" = no A/B)
    bool showingProcessed = false;         ///< B is the selected side
    std::optional<float> processedLufs;    ///< B's measured integrated LUFS
    /// B's loudness could not be measured (undecodable / silent file): play it
    /// unmatched rather than holding it muted forever.
    bool processedLufsUnavailable = false;
    float userVolume = 0.8f;               ///< transport volume 0..1
    float streamingPreviewOffsetDb = 0.0f; ///< platform-normalisation preview

    /// An audition preview that is neither A nor B (a device simulation,
    /// DSK-14): matched to A's loudness like B is, muted until measured.
    juce::String previewPath;
    std::optional<float> previewLufs;
    bool previewLufsUnavailable = false;

    /// STU-8: a version pinned as the A side ("" = the session's original,
    /// the default). B is matched to A's level, so A's level must be known:
    /// a pinned A is held muted - with B - until it is measured.
    juce::String referencePath;
    std::optional<float> referenceLufs;
    bool referenceLufsUnavailable = false;

    /// Forget everything session-scoped (user volume is an app preference).
    void resetForSession() noexcept
    {
        processedPath.clear();
        showingProcessed = false;
        processedLufs.reset();
        processedLufsUnavailable = false;
        clearReference();
        clearPreview();
    }

    void clearReference() noexcept
    {
        referencePath.clear();
        referenceLufs.reset();
        referenceLufsUnavailable = false;
    }

    /// STU-8: pins `path` as A ("" = back to the original). Returns true when
    /// its level is unknown and must be measured.
    bool assignReference(const juce::String& path, std::optional<float> cachedLufs) noexcept
    {
        clearReference();
        referencePath = path;
        referenceLufs = path.isNotEmpty() ? cachedLufs : std::nullopt;
        return path.isNotEmpty() && !cachedLufs.has_value();
    }

    /// A pinned A whose level is still being measured.
    bool awaitingReferenceLufs() const noexcept
    {
        return referencePath.isNotEmpty() && !referenceLufs.has_value() && !referenceLufsUnavailable;
    }

    /// The file playing as A: the pinned version, else the original.
    juce::String aSidePath(const juce::String& originalPath) const
    {
        return referencePath.isNotEmpty() ? referencePath : originalPath;
    }

    /// A's level: the pinned version's measured level, else the original's
    /// (the analysis).
    std::optional<float> aSideLufs(std::optional<float> originalLufs) const noexcept
    {
        return referencePath.isNotEmpty() ? referenceLufs : originalLufs;
    }

    void clearPreview() noexcept
    {
        previewPath.clear();
        previewLufs.reset();
        previewLufsUnavailable = false;
    }

    /// What the caller must do after assignProcessed().
    struct Assignment {
        bool reloadTransport = false; ///< B was the loaded side: reload it (or fall back to A)
        bool measure = false;         ///< B's level is unknown: measure it in the background
    };

    /// Makes `path` the B side. `loadedPath` is what the transport has loaded;
    /// `cachedLufs` the level already known for `path` (LufsCache), if any.
    /// The SAME path set again is treated as new audio - a re-render in place
    /// (AUDIT-018) - so it is re-levelled from the cache/measurement and, when
    /// B is what is playing, reloaded instead of the old early-return that
    /// left the transport on stale or torn audio.
    Assignment assignProcessed(const juce::String& path, const juce::String& loadedPath,
                               std::optional<float> cachedLufs, bool fileExists) noexcept
    {
        Assignment a;
        a.reloadTransport = showingProcessed && processedPath.isNotEmpty() && loadedPath == processedPath;
        processedPath = path;
        processedLufs = cachedLufs;
        processedLufsUnavailable = false;
        if (path.isEmpty() || !fileExists) showingProcessed = false;
        a.measure = fileExists && path.isNotEmpty() && !cachedLufs.has_value();
        return a;
    }

    /// True while B is the loaded side but its loudness is still being
    /// measured - the window in which gainFor() holds B muted.
    bool awaitingProcessedLufs() const noexcept
    {
        return processedPath.isNotEmpty() && !processedLufs.has_value() && !processedLufsUnavailable;
    }

    /// UI-6: B against A for the A/B control. "Matched" only when both
    /// levels are known - gainFor() can only even out what was measured.
    AbLevelOffset levelOffset(std::optional<float> originalLufs) const noexcept
    {
        AbLevelOffset o;
        o.available = processedPath.isNotEmpty();
        if (!o.available) return o;
        o.measuring = awaitingProcessedLufs() || awaitingReferenceLufs();
        const auto aLufs = aSideLufs(originalLufs);
        if (processedLufs.has_value() && aLufs.has_value()) {
            o.bMinusADb = *processedLufs - *aLufs;
            o.matched = true;
        }
        return o;
    }

    struct GainContext {
        std::optional<float> originalLufs;  ///< A's integrated LUFS (analysis)
        juce::String loadedPath;            ///< what the transport has loaded
        juce::String originalPath;          ///< the session's source file
    };

    /// Linear transport gain. Loudness-matches by ATTENUATING whichever of A/B
    /// is louder (never boosting past unity), keyed off the file actually
    /// loaded so stems / delta previews play unmatched.
    ///
    /// B is held MUTED until it is measured (AUDIT-038): the old genre-target
    /// guess could play an unmatched render several dB loud for seconds, which
    /// is exactly the "louder sounds better" bias matched A/B exists to remove.
    float gainFor(const GainContext& context) const noexcept
    {
        // STU-8: a pinned A replaces the original as the level both sides
        // are matched to.
        GainContext ctx = context;
        ctx.originalLufs = aSideLufs(context.originalLufs);
        ctx.originalPath = aSidePath(context.originalPath);
        float scale = 1.0f;
        const bool playingB = processedPath.isNotEmpty() && ctx.loadedPath == processedPath;
        if (playingB && awaitingProcessedLufs()) return 0.0f;
        const bool playingPinnedA = referencePath.isNotEmpty() && ctx.loadedPath == referencePath;
        if ((playingB || playingPinnedA) && awaitingReferenceLufs()) return 0.0f;
        const bool playingPreview = previewPath.isNotEmpty() && ctx.loadedPath == previewPath;
        if (playingPreview) {
            if (!previewLufs.has_value() && !previewLufsUnavailable) return 0.0f; // muted until measured
            if (previewLufs.has_value() && ctx.originalLufs.has_value()) {
                // Equal loudness with the mix: attenuate a louder preview, lift
                // a quieter one only as far as the volume headroom allows
                // (the final clamp keeps the gain at or below unity).
                scale = std::pow(10.0f, (*ctx.originalLufs - *previewLufs) / 20.0f);
            }
            return juce::jlimit(0.0f, 1.0f, userVolume * scale);
        }
        if (ctx.originalLufs.has_value() && processedLufs.has_value() && processedPath.isNotEmpty()) {
            const float diff = *processedLufs - *ctx.originalLufs; // B minus A, dB
            const bool playingA = ctx.originalPath.isNotEmpty() && ctx.loadedPath == ctx.originalPath;
            if (playingB && diff > 0.0f)
                scale = std::pow(10.0f, -diff / 20.0f);
            else if (playingA && diff < 0.0f)
                scale = std::pow(10.0f, diff / 20.0f);
        }
        if (streamingPreviewOffsetDb != 0.0f) scale *= std::pow(10.0f, streamingPreviewOffsetDb / 20.0f);
        return juce::jlimit(0.0f, 1.0f, userVolume * scale);
    }
};

} // namespace khensu
