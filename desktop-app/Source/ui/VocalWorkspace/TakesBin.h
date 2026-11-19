// desktop-app/Source/ui/VocalWorkspace/TakesBin.h
// The session's Takes bin (WF-5) as Comping, Take Alignment and Preflight
// pick from it: takes are loaded once (IVocalEdit::addSessionTakes, saved
// with the session) and offered to every panel instead of each opening its
// own file chooser.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../../services/IVocalEdit.h"
#include <functional>
#include <memory>

namespace khensu::takes_bin {

enum class Pick {
    Many, ///< a set of takes (Comping, Take Alignment): "Use all N" or a fresh selection
    One,  ///< a single recording (Preflight): one take from the bin, or one file
};

/// The label of the menu entry that uses the whole bin.
inline juce::String useAllText(int count)
{
    return TRANS("Use the session's %n takes").replace("%n", juce::String(count));
}

/// One take's menu label: its file name.
inline juce::String takeLabel(const juce::String& path) { return juce::File(path).getFileName(); }

/// Opens the file chooser; the files picked join the bin and go to `onPicked`
/// (nothing is called on cancel).
inline void browse(juce::Component& owner, IVocalEdit& edit, std::unique_ptr<juce::FileChooser>& chooser,
                   const juce::String& title, Pick mode, std::function<void(const juce::StringArray&)> onPicked)
{
    chooser = std::make_unique<juce::FileChooser>(title, juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                                                  "*.wav;*.aif;*.aiff;*.flac;*.mp3");
    int flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    if (mode == Pick::Many) flags |= juce::FileBrowserComponent::canSelectMultipleItems;
    juce::Component::SafePointer<juce::Component> alive(&owner);
    chooser->launchAsync(flags, [alive, &edit, onPicked = std::move(onPicked)](const juce::FileChooser& fc) {
        if (alive == nullptr) return;
        juce::StringArray picked;
        for (const auto& f : fc.getResults())
            if (f.existsAsFile()) picked.add(f.getFullPathName());
        if (picked.isEmpty()) return; // cancelled: keep what the panel had
        edit.addSessionTakes(picked);
        onPicked(picked);
    });
}

/// The pick: straight to the file chooser while the bin is empty; otherwise a
/// menu under `button` - "Use the session's N takes" (Many, two or more
/// takes) or each take (One), then "Add takes from disk...".
inline void pick(juce::Component& owner, juce::Component& button, IVocalEdit& edit,
                 std::unique_ptr<juce::FileChooser>& chooser, const juce::String& chooserTitle, Pick mode,
                 std::function<void(const juce::StringArray&)> onPicked)
{
    const auto takes = edit.sessionTakes();
    if (takes.isEmpty()) {
        browse(owner, edit, chooser, chooserTitle, mode, std::move(onPicked));
        return;
    }
    juce::PopupMenu menu;
    menu.addSectionHeader(TRANS("Session takes"));
    if (mode == Pick::Many) {
        menu.addItem(1, useAllText(takes.size()), takes.size() >= 2);
    } else {
        for (int i = 0; i < takes.size(); ++i)
            menu.addItem(100 + i, takeLabel(takes[i]));
    }
    menu.addSeparator();
    menu.addItem(2, TRANS("Add takes from disk..."));
    juce::Component::SafePointer<juce::Component> alive(&owner);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&button),
        [alive, &edit, &chooser, chooserTitle, mode, takes, onPicked = std::move(onPicked)](int id) mutable {
            if (alive == nullptr || id == 0) return;
            if (id == 1)
                onPicked(takes);
            else if (id == 2)
                browse(*alive, edit, chooser, chooserTitle, mode, std::move(onPicked));
            else if (id >= 100 && id - 100 < takes.size())
                onPicked(juce::StringArray(takes[id - 100]));
        });
}

} // namespace khensu::takes_bin
