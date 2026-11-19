// desktop-app/Source/ui/shared/SessionAudioPaths.h
#pragma once
#include <JuceHeader.h>
#include <map>

/// One home for the "which file do I process / where does the result go"
/// helpers that used to be copied per panel (audit 11.3: activeStemPaths()
/// existed twice, a vocal-stem-or-source targetFilePath() five times), plus
/// the render-scope rule that fixes UIX-01 / UIX-18 by construction.
///
/// Header-only and templated on the controller so it works with both
/// AppController and the IAppController seam the tests use.
namespace khensu::session_audio {

/// The separated stems that exist on disk for the active session, by name.
template <typename Controller>
std::map<juce::String, juce::String> activeStemPaths(const Controller& controller)
{
    std::map<juce::String, juce::String> paths;
    for (const char* name : {"vocals", "drums", "bass", "other"}) {
        const auto file = controller.getStemFile(name);
        if (file.existsAsFile()) paths.emplace(name, file.getFullPathName());
    }
    return paths;
}

/// The current vocal (WF-4): the edited vocal when one stacked, else the
/// isolated vocal stem when it exists, else the session's source mix (empty
/// with no session). The Vocal Workspace tools all process this, so their
/// edits stack on one vocal.
template <typename Controller>
juce::String vocalStemOrSourcePath(const Controller& controller)
{
    const juce::String edited = controller.currentVocalStem();
    if (edited.isNotEmpty()) return edited;
    const auto stem = controller.getStemFile("vocals");
    if (stem.existsAsFile()) return stem.getFullPathName();
    return controller.activeSourcePath();
}

/// The file Export writes - the accepted render (render-history current
/// node) when it exists on disk, else the session's original - through the
/// narrow controller seam (IAppController has no exportSourcePath()). Mirrors
/// RenderHistory::exportSource. PlatformMasters and Delivery QA process this
/// so "the current master" means the same file everywhere (UIX-06).
template <typename Controller>
juce::String exportSourcePath(const Controller& controller)
{
    const juce::String accepted = controller.currentRenderPath();
    if (accepted.isNotEmpty() && juce::File(accepted).existsAsFile()) return accepted;
    return controller.activeSourcePath();
}

/// What a finished render IS, which decides where it may go.
///  - Mix:  a new version of the whole mix - it may join the render history
///          and become what Export / Report / A/B use.
///  - Stem: a processed stem or single take - it is auditioned directly and
///          must never become the session's accepted "master" (UIX-01).
enum class RenderScope { Mix, Stem };

/// The scope of a render whose INPUT was `inputPath`: only a render of the
/// session's own source mix (or of the accepted mix render) is a Mix render.
/// Everything else - a separated stem, a user-chosen take - is a Stem render.
template <typename Controller>
RenderScope scopeForInput(const Controller& controller, const juce::String& inputPath)
{
    if (inputPath.isEmpty()) return RenderScope::Stem;
    const juce::File input(inputPath);
    const juce::String source = controller.activeSourcePath();
    if (source.isNotEmpty() && input == juce::File(source)) return RenderScope::Mix;
    const juce::String accepted = controller.currentRenderPath();
    if (accepted.isNotEmpty() && input == juce::File(accepted)) return RenderScope::Mix;
    return RenderScope::Stem;
}

/// True if `path` lies in the active session's stems directory.
template <typename Controller>
bool isInStemsDirectory(const Controller& controller, const juce::String& path)
{
    if (path.isEmpty()) return false;
    const juce::File stemsDir = controller.getStemFile("vocals").getParentDirectory();
    if (stemsDir == juce::File()) return false;
    return juce::File(path).isAChildOf(stemsDir);
}

/// Routes a finished render by scope. A Mix render registers into the
/// session's render history (session-checked, AUDIT-002); a Stem render is
/// loaded straight into the transport for auditioning and never touches the
/// render history, so Export, the Report and the mix A/B keep using the mix.
/// Returns true for Mix, false for Stem (callers word their status by it).
/// `inputPath` is what the render processed: a Mix render of an accepted
/// version is registered as that version's child, so fixes stack (WF-1); a
/// Stem render of the current vocal becomes the current vocal, so vocal edits
/// stack too (WF-4).
template <typename Controller>
bool applyRender(Controller& controller, const juce::String& renderedPath, const juce::String& sessionId,
                 RenderScope scope, const juce::String& inputPath = {})
{
    if (scope == RenderScope::Mix && !isInStemsDirectory(controller, renderedPath)) {
        if (inputPath.isNotEmpty())
            controller.renders().registerRenderOf(renderedPath, inputPath, sessionId);
        else
            controller.registerAppliedRender(renderedPath, sessionId);
        return true;
    }
    if (inputPath.isNotEmpty() && inputPath != controller.activeSourcePath() &&
        juce::File(inputPath) == juce::File(vocalStemOrSourcePath(controller)) && juce::File(renderedPath).existsAsFile())
        controller.stems().setCurrentVocalStem(renderedPath);
    controller.playback().loadAudioFileForPlayback(juce::File(renderedPath));
    return false;
}

} // namespace khensu::session_audio
