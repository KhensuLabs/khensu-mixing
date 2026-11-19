// desktop-app/Source/ui/SessionIntake/TargetSetupPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include <khensu/targets/CreativeTarget.h>
#include <map>
#include <string>
#include <vector>

namespace khensu {

class GenreDatabase;

// THREAD_CONTEXT(UIThread)
class TargetSetupPanel final : public juce::Component {
  public:
    explicit TargetSetupPanel(const GenreDatabase& genres);
    ~TargetSetupPanel() override = default;

    CreativeTarget buildTarget() const;
    void autoSelectGenre(const juce::String& genre, const juce::String& subgenre);

    /// Populates genre, sub-genre, mood, energy, BPM and key from a completed
    /// file analysis (Quick or Advanced mode). It never touches the hook,
    /// chorus payoff, heaviness or memorability sliders (TGT-1).
    void autoPopulateFromAnalysis(const juce::String& genre, const juce::String& subgenre, const juce::String& mood,
                                  float energy0to100, float bpm, const juce::String& key);

    /// Sets the four intent sliders (0-100; 50 = the genre's norm). They say
    /// what the user wants, so only the user's own choice sets them (a
    /// quick-start template, the sliders): filled from the song's own
    /// measurements, each recommender would judge the song against itself
    /// (TGT-1).
    void setIntentTargets(float hookEmphasis, float chorusPayoff, float heaviness, float memorability);

    /// Puts a detected genre in the genre box as a fill, not a statement:
    /// genreStatedByUser() stays as it was. Intake's Confirm uses it (NEW-9).
    void fillDetectedGenre(const juce::String& genreId)
    {
        fillingFromAnalysis_ = true;
        autoSelectGenre(genreId, {});
        fillingFromAnalysis_ = false;
    }

    void paint(juce::Graphics& g) override;
    void resized() override;

    /// True once the user picked the genre (the genre box, a quick-start
    /// template, a preset) - not when an analysis filled it in. A detected
    /// genre the user never confirmed must not count as stated (UX-12 / P0-1).
    bool genreStatedByUser() const noexcept { return genreStatedByUser_; }

    /// The playback-intent data id for an intent-combo item id (1-based);
    /// "" for none. The combo shows translated labels; the target stores ids
    /// (PlaybackIntentTargets.h compares them).
    static std::string playbackIntentIdForComboId(int comboId);

  private:
    bool genreStatedByUser_ = false;
    bool fillingFromAnalysis_ = false;
    // One-click starting points for a fresh session - set before a file is
    // even loaded, so the user can begin from a sane target instead of blanks.
    struct QuickStartTemplate {
        const char* label;
        const char* genre;
        const char* subgenre;
        const char* mood;
        float energy, bpm, hook, payoff, heaviness, memorability;
    };
    static const std::vector<QuickStartTemplate>& quickStartTemplates();
    void applyQuickStartTemplate(int index);

    juce::Label quickStartLabel;
    juce::ComboBox quickStartCombo;

    juce::Label headerLabel;

    juce::Label genreLabel;
    juce::ComboBox genreCombo; // populated from GenreDatabase
    juce::Label subGenreLabel;
    // Editable combo: dropdown of the selected genre's family members, plus a
    // "(none / unknown)" sentinel, and free typing for anything off-list.
    juce::ComboBox subGenreCombo;
    juce::Label moodLabel;
    juce::TextEditor moodEditor;

    juce::Label keyLabel;
    juce::Label keyValueLabel; // read-only - detected key isn't a user target

    juce::Label energyLabel;
    juce::Slider energySlider{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::Label bpmLabel;
    juce::Slider bpmSlider{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};

    juce::Label intentLabel;
    juce::ComboBox intentCombo;

    juce::Label hookLabel;
    juce::Slider hookSlider{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::Label payoffLabel;
    juce::Slider payoffSlider{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::Label heavyLabel;
    juce::Slider heavySlider{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::Label memoLabel;
    juce::Slider memoSlider{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};

    juce::Label visionLabel;
    juce::TextEditor visionEditor;
    juce::ToggleButton deepAnalysisToggle{TRANS("Enable AI Deep Analysis")};

    std::vector<juce::String> genreIds; // parallel to combo item order (1-based ids)

    // Sub-genre grouping, built once from the GenreDatabase at construction.
    std::map<std::string, std::string> genreIdToFamily_;              // genre id -> parent family label
    std::map<std::string, std::vector<juce::String>> familyToSubs_;   // family label -> member display names
    std::vector<juce::String> allSubNames_;                          // every genre name (used when no genre picked)

    static const char* kSubGenreNoneLabel; // sentinel meaning "unspecified"; maps to "" in buildTarget()

    // Repopulates subGenreCombo with the family of the given genre id (or all
    // genres when id is empty), keeping the "(none / unknown)" option first.
    void populateSubGenreForGenreId(const juce::String& genreId);

    void configurePct(juce::Slider& s, double initial);
    void configureRowLabel(juce::Label& l, const juce::String& text);
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TargetSetupPanel)
};

} // namespace khensu
