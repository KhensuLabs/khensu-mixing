// desktop-app/Source/ui/SessionIntake/IntakeBatchReport.cpp
#include "IntakeBatchReport.h"
#include "../shared/KhensuTheme.h"
#include "../shared/KhensuLookAndFeel.h"

namespace khensu {

namespace {

juce::Colour statusColour(const juce::String& status)
{
    if (status == "Analyzed") return KhensuTheme::accentGreen;
    if (status == "Failed") return KhensuTheme::accentRed;
    if (status == "Cancelled") return KhensuTheme::accentAmber;
    return KhensuTheme::textTertiary;
}

} // namespace

// Every cached palette colour on this component, in one place (UI-01).
void IntakeBatchReport::applyThemeColours()
{
    titleLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    summaryLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    closeButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    closeButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
}

IntakeBatchReport::IntakeBatchReport(const juce::String& sessionName, const std::vector<Item>& items) : items_(items)
{
    applyThemeColours();
    titleLabel_.setText(TRANS("Intake report: ") + sessionName, juce::dontSendNotification);
    titleLabel_.setFont(KhensuTheme::labelFont(16.0f));
    addAndMakeVisible(titleLabel_);

    int analyzed = 0, failed = 0, cancelled = 0, pending = 0;
    for (const auto& it : items) {
        if (it.status == "Analyzed")
            ++analyzed;
        else if (it.status == "Failed")
            ++failed;
        else if (it.status == "Cancelled")
            ++cancelled;
        else
            ++pending;
    }
    juce::String summary = "Total: " + juce::String((int)items.size()) + "  |  Analyzed: " + juce::String(analyzed) +
                           "  |  Failed: " + juce::String(failed) + "  |  Cancelled: " + juce::String(cancelled);
    if (pending > 0) summary += "  |  Pending: " + juce::String(pending);
    summaryLabel_.setText(summary, juce::dontSendNotification);
    summaryLabel_.setFont(KhensuTheme::bodyFont(12.0f));
    addAndMakeVisible(summaryLabel_);

    closeButton_.onClick = [this] {
        if (onClose) onClose();
    };
    addAndMakeVisible(closeButton_);

    addAndMakeVisible(viewport_);
    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);

    const int rowH = 32;
    content_.setSize(100, (int)items_.size() * rowH);
    for (const auto& item : items_) {
        auto* label = new juce::Label();
        juce::String name =
            item.file.title.empty() ? juce::File(item.file.path).getFileName() : juce::String(item.file.title);
        label->setText(name + "  -  " + item.status, juce::dontSendNotification);
        label->setFont(KhensuTheme::bodyFont(12.0f));
        label->setColour(juce::Label::textColourId, statusColour(item.status));
        content_.addAndMakeVisible(label);
        rowLabels_.push_back(label);
    }
}

IntakeBatchReport::~IntakeBatchReport() = default;

void IntakeBatchReport::paint(juce::Graphics& g)
{
    // Dim the screen behind. Uses the shared scrim token so modal backdrops
    // stay consistent across themes (glassmorphism token sweep).
    g.fillAll(KhensuTheme::scrim);

    auto b = getLocalBounds().reduced(getWidth() / 6, getHeight() / 8).toFloat();
    KhensuLookAndFeel::drawGlassPanel(g, b.toNearestInt(), GlassPanelVariant::Raised);
}

void IntakeBatchReport::resized()
{
    auto b = getLocalBounds().reduced(getWidth() / 6, getHeight() / 8);

    auto header = b.removeFromTop(56).reduced(16, 0);
    titleLabel_.setBounds(header.removeFromTop(28));
    summaryLabel_.setBounds(header);

    auto footer = b.removeFromBottom(44).reduced(16, 8);
    closeButton_.setBounds(footer.removeFromRight(100));

    b.reduce(16, 8);
    viewport_.setBounds(b);

    const int rowH = 32;
    content_.setSize(b.getWidth(), juce::jmax(b.getHeight(), (int)items_.size() * rowH));

    for (size_t i = 0; i < rowLabels_.size(); ++i)
        rowLabels_[i]->setBounds(0, (int)i * rowH, content_.getWidth(), rowH);
}

} // namespace khensu
