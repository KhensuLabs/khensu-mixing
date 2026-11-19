// desktop-app/Source/ui/ReferenceLibrary/ReferenceProfilesBar.h
// CE-COL-4: shared reference profiles in the Reference Library - analyse a
// reference track into numbers (never audio), export / import hashed profile
// files, and browse an optional shared index (online-gated by the service).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/ExperimentalBadge.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/ILocalServiceBridge.h>
#include <functional>
#include <string>
#include <vector>

namespace khensu {

class IBackgroundWork;

class ReferenceProfilesBar final : public juce::Component {
  public:
    struct UserProfile {
        juce::String name;
        ipc::GenreReferencePoint point;
    };

    ReferenceProfilesBar(IBackgroundWork& controller, std::function<juce::String()> selectedGenre,
                         std::string baseUrl = ipc::HttpServiceBridge::kDefaultBaseUrl);
    ~ReferenceProfilesBar() override;

    /// Reload the user's profiles for `genreId` (GET /references/profiles).
    void refresh(const juce::String& genreId);
    /// Fires on the message thread with the user's profiles for the genre.
    std::function<void(const juce::String& genreId, std::vector<UserProfile>)> onProfilesChanged;

    void resized() override;

  private:
    void analyseReference();
    void exportProfiles();
    void importProfiles();
    void browseIndex();
    void fetchIndexEntry(const juce::String& url, const juce::String& sha256);
    void runPost(const std::string& route, juce::var body, std::function<void(const juce::var&)> onOk);
    void setStatus(const juce::String& text);

    IBackgroundWork& controller_;
    std::function<juce::String()> selectedGenre_;
    std::string baseUrl_;
    juce::TextButton analyseButton_{TRANS("Analyse reference...")};
    juce::TextButton exportButton_{TRANS("Export profiles...")};
    juce::TextButton importButton_{TRANS("Import profiles...")};
    juce::TextButton browseButton_{TRANS("Browse index...")};
    juce::Label statusLabel_;
    ExperimentalBadge experimentalBadge_; ///< VER-8
    std::unique_ptr<juce::FileChooser> chooser_;
    std::unique_ptr<juce::AlertWindow> urlDialog_;
    juce::String lastGenre_;

    void applyThemeColours();
    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReferenceProfilesBar)
};

} // namespace khensu
