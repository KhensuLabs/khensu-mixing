// desktop-app/Source/ui/ReferenceLibrary/SimilarReferencesPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/ExperimentalBadge.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include <khensu/ipc/HttpServiceBridge.h>
#include <memory>
#include <string>
#include <vector>

namespace khensu {

class IAppController;

/// CE-AI-1 "Find similar": the user's own reference files, indexed locally
/// with audio embeddings (CLAP when its weights are cached, else the
/// deterministic hand-built embedding - the backend and why is always shown),
/// and ranked by similarity to the loaded track. A result can be made the
/// session's reference track (the same path ReferenceComparePanel loads).
/// THREAD_CONTEXT(UIThread)
class SimilarReferencesPanel final : public juce::Component, private juce::ListBoxModel {
  public:
    explicit SimilarReferencesPanel(IAppController& controller, std::string baseUrl = ipc::HttpServiceBridge::kDefaultBaseUrl);
    ~SimilarReferencesPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Clears results that belonged to the previous session.
    void resetForSession();
    /// FEAT-5: list every reference in the library (GET /embeddings/index)
    /// and show which one the session uses. Called when the screen opens.
    void showLibrary();

  private:
    struct Row {
        juce::String id, path, label; ///< id: the reference index entry
        double similarity = 0.0;
        bool exists = true;
        juce::StringArray tags; ///< FEAT-5: the user's tags and genre
        juce::String genre;
    };

    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void selectedRowsChanged(int lastRowSelected) override;

    void addReferences();
    void findSimilar();
    /// NEW-5: the references closest to a typed description (CLAP text).
    void searchByDescription();
    void showMatches(const juce::var& response);
    void useSelectedAsReference();
    /// Drops the selected entry from the reference index (a moved or deleted
    /// file, or one the user no longer wants matched).
    void removeSelected();
    void setBusy(bool busy, const juce::String& status);
    /// FEAT-5: edit the selected entry's tags and genre.
    void editSelectedTags();
    /// Rebuilds the filter list from the library's tags and genres.
    void rebuildFilterChoices();
    ipc::HttpServiceBridge::EmbeddingFilter currentFilter() const;
    static Row rowFrom(const juce::var& entry, double similarity);

    IAppController& controller_;
    std::string baseUrl_;
    juce::Label titleLabel_;
    ExperimentalBadge experimentalBadge_; ///< VER-8
    juce::Label statusLabel_;
    juce::Label sessionRefLabel_; ///< FEAT-5: the session's reference track
    juce::TextButton libraryButton_{TRANS("All references")};
    void refreshSessionReference();
    juce::TextButton addButton_{TRANS("Add references...")};
    juce::TextButton findButton_{TRANS("Find similar")};
    juce::TextButton useButton_{TRANS("Use as reference")};
    juce::TextButton removeButton_{TRANS("Remove")};
    juce::TextButton tagsButton_{TRANS("Tags...")};
    juce::ComboBox filterBox_; ///< FEAT-5: All, then each genre and tag in the library
    /// Filter choices by combo id - 1: {isGenre, value}.
    std::vector<std::pair<bool, juce::String>> filterChoices_;
    std::unique_ptr<juce::AlertWindow> tagsDialog_;
    juce::TextEditor describeEditor_;
    juce::TextButton describeButton_{TRANS("Search")};
    juce::ListBox list_;
    std::vector<Row> rows_;
    std::unique_ptr<juce::FileChooser> chooser_;
    bool busy_ = false;

    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SimilarReferencesPanel)
};

} // namespace khensu
