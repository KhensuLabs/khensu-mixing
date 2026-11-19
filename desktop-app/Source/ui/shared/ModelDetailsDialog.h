// desktop-app/Source/ui/shared/ModelDetailsDialog.h
// Rich detail view of the bundled learned models: per-model version, provenance,
// validation status, size, sha256, and downloaded-at, plus a "Check for updates"
// action that compares each model against its sidecar source_url. All bridge
// traffic runs off the message thread. THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <functional>
#include "KhensuTheme.h"

namespace khensu {

class ModelDetailsDialog final : public juce::Component {
  public:
    /// `runInBackground` schedules the dialog's bridge requests; pass the
    /// controller's owned pool (AppController::runInBackground) so no request
    /// outlives the app the way a detached juce::Thread::launch could.
    using BackgroundRunner = std::function<void(std::function<void()>)>;
    explicit ModelDetailsDialog(BackgroundRunner runInBackground);
    void resized() override;

  private:
    void refreshManifest();
    void checkForUpdates();

    juce::TextButton checkButton_{TRANS("Check for updates")};
    juce::TextButton closeButton_{TRANS("Close")};
    juce::Label headerLabel_;
    juce::TextEditor body_; // multi-line, read-only detail list
    bool requestInFlight_ = false;
    BackgroundRunner runInBackground_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModelDetailsDialog)
};

} // namespace khensu
