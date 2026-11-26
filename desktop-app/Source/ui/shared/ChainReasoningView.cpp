// desktop-app/Source/ui/shared/ChainReasoningView.cpp
#include "ChainReasoningView.h"
#include "KhensuLookAndFeel.h"

namespace khensu {

// ----- Section -----

ChainReasoningView::Section::Section(const juce::String& label, juce::Colour accent) : label_(label), accent_(accent)
{
    headerButton_.setButtonText(">  " + label_);
    applyThemeColours();
    headerButton_.onClick = [this]() {
        setExpanded(!expanded_);
        if (onToggled) onToggled();
    };
    addAndMakeVisible(headerButton_);

    bodyLabel_.setFont(khensu::KhensuTheme::uiFont(khensu::type::secondary, juce::Font::plain));
    bodyLabel_.setJustificationType(juce::Justification::topLeft);
    bodyLabel_.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(bodyLabel_);
}

// Every cached palette colour on a section, in one place (UI-01). Sections are
// per-row child components, so each one owns its own listener rather than
// relying on the parent to rebuild them - a theme switch with a static list
// would otherwise never reach them. accent_ is caller-supplied (a severity or
// category colour), so it is re-read from the member, not re-derived here.
void ChainReasoningView::Section::applyThemeColours()
{
    headerButton_.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    headerButton_.setColour(juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    headerButton_.setColour(juce::TextButton::textColourOffId, accent_);
    headerButton_.setColour(juce::TextButton::textColourOnId, accent_);
    bodyLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
}

void ChainReasoningView::Section::setBody(const juce::String& text)
{
    hasContent_ = text.isNotEmpty();
    bodyLabel_.setText(hasContent_ ? text : "(not provided)", juce::dontSendNotification);
    setVisible(hasContent_);
}

void ChainReasoningView::Section::setExpanded(bool shouldExpand)
{
    expanded_ = shouldExpand;
    headerButton_.setButtonText((expanded_ ? juce::String("v  ") : juce::String(">  ")) + label_);
    bodyLabel_.setVisible(expanded_);
}

int ChainReasoningView::Section::preferredHeight() const
{
    if (!hasContent_) return 0;
    if (!expanded_) return kHeaderH;
    // Rough word-wrapped height estimate at the last-known width.
    const int w = juce::jmax(80, getWidth() - 16);
    const int lines =
        juce::jmax(1, juce::GlyphArrangement::getStringWidthInt(bodyLabel_.getFont(), bodyLabel_.getText()) / w + 1);
    return kHeaderH + juce::jmax(20, lines * 16) + 8;
}

void ChainReasoningView::Section::resized()
{
    auto b = getLocalBounds();
    headerButton_.setBounds(b.removeFromTop(kHeaderH));
    if (expanded_) {
        b.removeFromTop(2);
        bodyLabel_.setBounds(b.reduced(14, 0));
    }
}

void ChainReasoningView::Section::paint(juce::Graphics& g)
{
    if (expanded_ && hasContent_) {
        g.setColour(accent_.withAlpha(0.06f));
        g.fillRoundedRectangle(getLocalBounds().toFloat().withTrimmedTop((float)kHeaderH), 3.0f);
    }
}

// ----- ChainReasoningView -----

ChainReasoningView::ChainReasoningView()
{
    struct Def {
        const char* label;
    };
    static constexpr Def kDefs[8] = {
        {"In plain words"}, {"Myth check"}, {"Detected"}, {"Cause"}, {"Fix"}, {"Trade-off"},
        {"Alternative Strategies"}, {"How to Verify"},
    };
    const juce::Colour colours[8] = {
        KhensuTheme::accentBlue, KhensuTheme::accentAmber, KhensuTheme::accentRed,  KhensuTheme::accentAmber,
        KhensuTheme::accentTeal, KhensuTheme::accentAmber, KhensuTheme::accentTeal, KhensuTheme::accentTeal,
    };
    for (int i = 0; i < 8; ++i) {
        sections_[(size_t)i] = std::make_unique<Section>(kDefs[i].label, colours[i]);
        sections_[(size_t)i]->onToggled = [this]() {
            resized();
            if (onHeightChanged) onHeightChanged();
        };
        addAndMakeVisible(sections_[(size_t)i].get());
    }
    // Plain words + myth check (when present), Detected and Fix expanded by
    // default; everything else is one click away. The first two stay hidden
    // until setPlainLanguage() supplies text.
    sections_[0]->setExpanded(true);
    sections_[1]->setExpanded(true);
    sections_[2]->setExpanded(true);
    sections_[4]->setExpanded(true);
    sections_[0]->setBody({});
    sections_[1]->setBody({});
}

void ChainReasoningView::setRecommendation(const Recommendation& rec)
{
    sections_[1]->setBody(rec.is_myth_adjacent ? juce::String(rec.myth_clarification) : juce::String());
    sections_[2]->setBody(juce::String(rec.what));
    sections_[3]->setBody(juce::String(rec.most_likely_cause));
    sections_[4]->setBody(juce::String(rec.suggested_action));
    sections_[5]->setBody(juce::String(rec.trade_off));

    juce::String alts;
    for (size_t i = 0; i < rec.alternative_strategies.size(); ++i) {
        if (i > 0) alts += "\n\n";
        alts +=
            juce::String::charToString((juce::juce_wchar)0x2022) + " " + juce::String(rec.alternative_strategies[i]);
    }
    sections_[6]->setBody(alts);
    sections_[7]->setBody(juce::String(rec.how_to_verify));

    resized();
    if (onHeightChanged) onHeightChanged();
}

void ChainReasoningView::setPlainLanguage(const juce::String& explanation, const juce::String& mythClarification,
                                          const juce::String& cause, const juce::String& tradeOff,
                                          const juce::String& howToVerify)
{
    sections_[0]->setBody(explanation);
    if (mythClarification.isNotEmpty()) sections_[1]->setBody(mythClarification);
    const auto fillIfEmpty = [](Section& s, const juce::String& text) {
        if (!s.isVisible() && text.isNotEmpty()) s.setBody(text);
    };
    fillIfEmpty(*sections_[3], cause);
    fillIfEmpty(*sections_[5], tradeOff);
    fillIfEmpty(*sections_[7], howToVerify);
    resized();
    if (onHeightChanged) onHeightChanged();
}

int ChainReasoningView::getContentHeight() const
{
    int y = 0;
    for (const auto& s : sections_)
        if (s->isVisible()) y += s->preferredHeight() + kSectionGap;
    return y;
}

void ChainReasoningView::resized()
{
    int y = 0;
    const int w = getWidth();
    for (auto& s : sections_) {
        if (!s->isVisible()) {
            s->setBounds(0, y, w, 0);
            continue;
        }
        const int h = s->preferredHeight();
        s->setBounds(0, y, w, h);
        y += h + kSectionGap;
    }
}

void ChainReasoningView::paint(juce::Graphics&) {}

} // namespace khensu
