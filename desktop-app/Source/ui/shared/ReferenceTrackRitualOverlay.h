// desktop-app/Source/ui/shared/ReferenceTrackRitualOverlay.h
// Reference-Track Ritual (kimi audit P2): a first-class modal for loading a
// reference track, viewing delta metrics, applying match-EQ, and A/B-ing the
// result. Replaces the hidden ReferenceComparePanel workflow with a guided,
// trust-building ritual.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h"

namespace khensu {

class ReferenceTrackRitualOverlay final : public juce::Component, public juce::DragAndDropTarget {
  public:
    ReferenceTrackRitualOverlay()
    {
        applyThemeColours();
        addAndMakeVisible(titleLabel_);
        addAndMakeVisible(detailLabel_);
        addAndMakeVisible(dropLabel_);
        addAndMakeVisible(currentRefLabel_);
        addAndMakeVisible(browseButton_);
        addAndMakeVisible(matchEqButton_);
        addAndMakeVisible(abButton_);
        addAndMakeVisible(clearButton_);
        addAndMakeVisible(closeButton_);

        titleLabel_.setText(TRANS("REFERENCE-TRACK RITUAL"), juce::dontSendNotification);
        titleLabel_.setFont(KhensuTheme::labelFont(type::secondary).boldened());
        titleLabel_.setJustificationType(juce::Justification::centred);

        detailLabel_.setText(
            TRANS("Drop a reference master to compare loudness, dynamics, spectral balance, and stereo width."),
            juce::dontSendNotification);
        detailLabel_.setFont(KhensuTheme::bodyFont(type::secondary));
        detailLabel_.setJustificationType(juce::Justification::centred);

        dropLabel_.setText(TRANS("Drop reference file here"), juce::dontSendNotification);
        dropLabel_.setFont(KhensuTheme::metricFont(type::heading));
        dropLabel_.setJustificationType(juce::Justification::centred);

        currentRefLabel_.setFont(KhensuTheme::bodyFont(type::caption));
        currentRefLabel_.setJustificationType(juce::Justification::centred);

        browseButton_.setButtonText(TRANS("Browse..."));
        matchEqButton_.setButtonText(TRANS("Apply match-EQ"));
        abButton_.setButtonText(TRANS("A/B compare"));
        clearButton_.setButtonText(TRANS("Clear"));
        closeButton_.setButtonText(TRANS("Close"));


        browseButton_.onClick = [this] { openFileChooser(); };
        matchEqButton_.onClick = [this] {
            if (onApplyMatchEq) onApplyMatchEq(currentRefPath_);
        };
        abButton_.onClick = [this] {
            if (onAbCompare) onAbCompare();
        };
        clearButton_.onClick = [this] {
            currentRefPath_.clear();
            refresh();
            if (onReferenceCleared) onReferenceCleared();
        };
        closeButton_.onClick = [this] {
            if (onClose) onClose();
        };

        setInterceptsMouseClicks(true, true);
    }

    std::function<void(const juce::String&)> onReferenceDropped;
    std::function<void()> onReferenceCleared;
    std::function<void(const juce::String&)> onApplyMatchEq;
    std::function<void()> onAbCompare;
    std::function<void()> onClose;

    void setCurrentReference(const juce::String& path)
    {
        currentRefPath_ = path;
        refresh();
    }

    bool isInterestedInDragSource(const SourceDetails& dragSourceDetails) override
    {
        return dragSourceDetails.description.toString().startsWith("file:");
    }

    void itemDragEnter(const SourceDetails&) override
    {
        dragHover_ = true;
        repaint();
    }
    void itemDragExit(const SourceDetails&) override
    {
        dragHover_ = false;
        repaint();
    }

    void itemDropped(const SourceDetails& dragSourceDetails) override
    {
        dragHover_ = false;
        auto files = juce::StringArray::fromTokens(dragSourceDetails.description.toString(), ";", {});
        for (const auto& f : files) {
            juce::File file(f.trim().unquoted());
            if (isAudioFile(file)) {
                currentRefPath_ = file.getFullPathName();
                if (onReferenceDropped) onReferenceDropped(currentRefPath_);
                refresh();
                break;
            }
        }
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(KhensuTheme::scrim);

        auto card = cardBounds().toFloat();
        g.setColour(KhensuTheme::bgRaised.withAlpha(0.96f));
        g.fillRoundedRectangle(card, 12.0f);
        g.setColour(KhensuTheme::borderActive);
        g.drawRoundedRectangle(card, 12.0f, 1.0f);
        g.setColour(KhensuTheme::accentBlue);
        g.fillRoundedRectangle(card.getX(), card.getY(), card.getWidth(), 2.0f, 1.0f);

        auto drop = dropZoneBounds().toFloat();
        g.setColour(dragHover_ ? KhensuTheme::accentBlue.withAlpha(0.22f) : KhensuTheme::bgSurface.withAlpha(0.6f));
        g.fillRoundedRectangle(drop, 8.0f);
        g.setColour(dragHover_ ? KhensuTheme::accentBlue : KhensuTheme::borderSubtle);
        g.drawRoundedRectangle(drop, 8.0f, 1.5f);

        // Dashed-line hint (manual dash - strokeDashedPath is not available in JUCE 7).
        g.setColour(KhensuTheme::borderActive);
        auto r = drop.reduced(8.0f);
        const float dash = 6.0f;
        auto drawDashedLine = [&](juce::Point<float> a, juce::Point<float> b) {
            const float len = a.getDistanceFrom(b);
            if (len < 1.0f) return;
            const int segments = (int)std::ceil(len / (dash * 2.0f));
            const juce::Point<float> step = (b - a) / (float)segments;
            for (int i = 0; i < segments; ++i) {
                const auto s = a + step * (float)i;
                const auto e = a + step * (float)(i + 0.5f);
                g.drawLine(s.x, s.y, e.x, e.y, 1.0f);
            }
        };
        drawDashedLine(r.getTopLeft(), r.getTopRight());
        drawDashedLine(r.getTopRight(), r.getBottomRight());
        drawDashedLine(r.getBottomRight(), r.getBottomLeft());
        drawDashedLine(r.getBottomLeft(), r.getTopLeft());
    }

    void resized() override
    {
        auto card = cardBounds();
        titleLabel_.setBounds(card.removeFromTop(30).reduced(0, 4));
        card.removeFromTop(8);
        detailLabel_.setBounds(card.removeFromTop(36));
        card.removeFromTop(16);

        dropLabel_.setBounds(dropZoneBounds());
        currentRefLabel_.setBounds(dropZoneBounds().expanded(0, 30));

        card.removeFromTop(dropZoneBounds().getHeight() + 50);

        auto buttons = card.removeFromBottom(36);
        closeButton_.setBounds(buttons.removeFromLeft(70).withHeight(28));
        buttons.removeFromLeft(8);
        clearButton_.setBounds(buttons.removeFromLeft(70).withHeight(28));
        buttons.removeFromLeft(8);
        browseButton_.setBounds(buttons.removeFromLeft(90).withHeight(28));
        buttons.removeFromLeft(12);
        abButton_.setBounds(buttons.removeFromLeft(100).withHeight(28));
        buttons.removeFromLeft(8);
        matchEqButton_.setBounds(buttons.removeFromLeft(120).withHeight(28));
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (!cardBounds().contains(e.getPosition()) && onClose) onClose();
    }

  private:
    /// Every cached palette colour on this overlay, in one place (UI-01).
    void applyThemeColours()
    {
        titleLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
        detailLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
        dropLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
        currentRefLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
        browseButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgRaised);
        browseButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textPrimary);
        matchEqButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.22f));
        matchEqButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
        abButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentBlue.withAlpha(0.22f));
        abButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentBlue);
        clearButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
        closeButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
    }

    juce::Rectangle<int> cardBounds() const
    {
        const int w = juce::jmin(520, getWidth() - 40);
        const int h = juce::jmin(420, getHeight() - 40);
        return juce::Rectangle<int>(w, h).withCentre(getLocalBounds().getCentre());
    }

    juce::Rectangle<int> dropZoneBounds() const
    {
        auto card = cardBounds();
        card.removeFromTop(74);
        return card.removeFromTop(140).reduced(30, 10);
    }

    void refresh()
    {
        const bool hasRef = currentRefPath_.isNotEmpty();
        matchEqButton_.setEnabled(hasRef);
        abButton_.setEnabled(hasRef);
        clearButton_.setEnabled(hasRef);
        currentRefLabel_.setText(hasRef ? "Loaded: " + juce::File(currentRefPath_).getFileName() : juce::String(),
                                 juce::dontSendNotification);
    }

    static bool isAudioFile(const juce::File& f) { return f.hasFileExtension("wav;aiff;flac;mp3;aac;ogg"); }

    void openFileChooser()
    {
        fileChooser_ = std::make_unique<juce::FileChooser>(
            "Choose a reference track", juce::File::getSpecialLocation(juce::File::userMusicDirectory),
            "*.wav;*.aiff;*.flac;*.mp3;*.aac;*.ogg");
        fileChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& fc) {
                                      auto f = fc.getResult();
                                      if (isAudioFile(f)) {
                                          currentRefPath_ = f.getFullPathName();
                                          if (onReferenceDropped) onReferenceDropped(currentRefPath_);
                                          refresh();
                                      }
                                  });
    }

    juce::String currentRefPath_;
    bool dragHover_ = false;

    juce::Label titleLabel_;
    juce::Label detailLabel_;
    juce::Label dropLabel_;
    juce::Label currentRefLabel_;
    juce::TextButton browseButton_;
    juce::TextButton matchEqButton_;
    juce::TextButton abButton_;
    juce::TextButton clearButton_;
    juce::TextButton closeButton_;
    std::unique_ptr<juce::FileChooser> fileChooser_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReferenceTrackRitualOverlay)
};

} // namespace khensu
