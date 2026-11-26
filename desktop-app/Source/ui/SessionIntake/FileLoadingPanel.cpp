#include "FileLoadingPanel.h"
#include "../shared/KhensuLookAndFeel.h"

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void FileLoadingPanel::applyThemeColours()
{
    headerLabel.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
    nameLabel.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    detailLabel.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

FileLoadingPanel::FileLoadingPanel()
{
    applyThemeColours();
    headerLabel.setText(TRANS("LOADED FILE"), juce::dontSendNotification);
    headerLabel.setFont(khensu::KhensuTheme::uiFont(khensu::type::body, juce::Font::bold));
    addAndMakeVisible(headerLabel);

    nameLabel.setFont(khensu::KhensuTheme::uiFont(khensu::type::subtitle, juce::Font::bold));
    addAndMakeVisible(nameLabel);

    detailLabel.setFont(khensu::KhensuTheme::uiFont(khensu::type::body, juce::Font::plain));
    detailLabel.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(detailLabel);

    clear();
}

void FileLoadingPanel::showFile(const AudioFileRef& ref)
{
    hasFile = true;
    juce::File f(juce::String(ref.path));
    nameLabel.setText(f.getFileName(), juce::dontSendNotification);

    auto mins = (int)(ref.duration_seconds / 60.0);
    auto secs = (int)(ref.duration_seconds) % 60;
    juce::String detail;
    detail << juce::String(ref.format).toUpperCase() << "  -  " << juce::String(mins) << ":"
           << juce::String(secs).paddedLeft('0', 2) << "  -  " << juce::String(ref.sample_rate / 1000.0, 1)
           << " kHz  -  " << juce::String(ref.bit_depth) << "-bit  -  "
           << (ref.channels == 1   ? "Mono"
               : ref.channels == 2 ? "Stereo"
                                   : juce::String(ref.channels) + " ch");
    detailLabel.setText(detail, juce::dontSendNotification);
    setVisible(true);
    repaint();
}

void FileLoadingPanel::clear()
{
    hasFile = false;
    nameLabel.setText(TRANS("No file selected"), juce::dontSendNotification);
    detailLabel.setText("", juce::dontSendNotification);
    repaint();
}

void FileLoadingPanel::paint(juce::Graphics& g)
{
    g.setColour(KhensuTheme::bgSurface.withAlpha(hasFile ? 0.5f : 0.2f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
}

void FileLoadingPanel::resized()
{
    auto b = getLocalBounds().reduced(16);
    headerLabel.setBounds(b.removeFromTop(18));
    b.removeFromTop(6);
    nameLabel.setBounds(b.removeFromTop(22));
    b.removeFromTop(4);
    detailLabel.setBounds(b.removeFromTop(20));
}

} // namespace khensu
