// desktop-app/Source/ui/Dashboard/WaveformEditorPanel.h
// Waveform & spectrogram editing view: a zoomable time view of the loaded
// audio with scrub-to-seek, range selection, brush-to-zoom, an overview
// strip, and time-ranged text annotations persisted per audio file.
//   Mouse: click = seek/scrub, drag = select, Shift+click = extend,
//          Ctrl+wheel = zoom, wheel = scroll, double-click a note = edit.
//   Keys:  Space play/pause, Left/Right move 1 s (Shift extends selection),
//          +/- zoom, 0 fit, Z zoom to selection, N add note, Delete remove
//          note at playhead, [ / ] previous/next note, Esc clear selection,
//          E explain the selection (CE-AI-6),
//          S toggle spectrogram.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/EditViewState.h"
#include "../shared/SessionCommentsLane.h"

namespace khensu {

class IAppController;

class WaveformEditorPanel final : public PanelCard {
  public:
    explicit WaveformEditorPanel(IAppController& c);
    ~WaveformEditorPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    bool keyPressed(const juce::KeyPress& key) override;

  private:
    /// CE-PLT-4: display-rate updates (VBlankAttachment). Repaints only the
    /// playhead's old and new strips unless the view scrolled.
    void onVBlank(double timestampSec);
    void syncSource();
    void computeSpectrogram(const juce::String& path);
    void promptForAnnotation(int editId);
    void saveAnnotations();
    void loadAnnotations();
    juce::String annotationKey() const;
    void updateStatus();
    void seek(double t);

    juce::Rectangle<float> mainArea() const;
    juce::Rectangle<float> overviewArea() const;

    EditViewState state_;
    juce::String sourcePath_;
    std::vector<float> peaks_;
    std::vector<std::vector<float>> spectroFrames_; ///< [frame][band] 0..1, whole file
    juce::Image spectroImage_;
    bool showSpectrogram_ = false;
    bool spectroPending_ = false;
    double spectroMaxHz_ = 22050.0;

    enum class Drag { None, Select, Overview } drag_ = Drag::None;
    double dragAnchor_ = 0.0;
    float hoverX_ = -1.0f, hoverY_ = -1.0f;

    juce::TextButton spectroButton_{TRANS("Spectrogram")};
    juce::TextButton zoomInButton_{"+"};
    juce::TextButton zoomOutButton_{"-"};
    juce::TextButton fitButton_{TRANS("Fit")};
    juce::TextButton zoomSelButton_{TRANS("Zoom to Selection")};
    juce::TextButton noteButton_{TRANS("Add Note")};
    /// CE-AI-6: "Explain this moment" for the current selection.
    juce::TextButton explainButton_{TRANS("Explain")};
    void explainSelection();
    juce::Label statusLabel_;
    /// CE-COL-1: shared, session-level timestamped comments (under the main view).
    SessionCommentsLane commentsLane_;
    std::unique_ptr<juce::AlertWindow> noteDialog_;
    double lastSourceSyncSec_ = -1.0;
    // Touches state_/controller_: declared after them so it detaches first.
    juce::VBlankAttachment vblank_{this, [this](double t) { onVBlank(t); }};

    void applyThemeColours();
    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformEditorPanel)
};

} // namespace khensu
