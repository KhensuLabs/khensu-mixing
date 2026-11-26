#include "RecentSessionsPanel.h"
#include "../shared/KhensuTheme.h"
#include "../shared/KhensuLookAndFeel.h"

namespace khensu {

// ── SessionCard ──────────────────────────────────────────────────────────────
// Private card component for horizontal scroll layout.
// Defined locally in the .cpp - not exposed through the header.

struct SessionCard final : public juce::Component, public juce::SettableTooltipClient {
    juce::String name_, subtitle_, note_;
    std::function<void()> onClick_;
    std::function<void()> onMenu_; // WF-12: right-click

    SessionCard(const Session& s, std::function<void()> cb, std::function<void()> menu)
        : onClick_(std::move(cb)), onMenu_(std::move(menu))
    {
        name_ = juce::String(s.name);
        note_ = juce::String::fromUTF8(s.note.c_str());
        setTooltip(TRANS("Click to open; right-click to rename, add a version note or duplicate"));
        juce::String sub;
        if (!s.target.primary_genre.empty()) sub << juce::String(s.target.primary_genre) << juce::String(juce::CharPointer_UTF8("  \xE2\x80\xA2  "));
        sub << juce::String(s.created_at).substring(0, 10); // ISO date yyyy-mm-dd
        subtitle_ = sub;
    }

    void paint(juce::Graphics& g) override
    {
        KhensuLookAndFeel::drawGlassPanel(g, getLocalBounds(), GlassPanelVariant::Raised);
        auto b = getLocalBounds().reduced(KhensuTheme::sp2, KhensuTheme::sp2);

        g.setFont(KhensuTheme::bodyFont(type::body));
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(name_, b.removeFromTop(22), juce::Justification::centredLeft, true);

        g.setFont(KhensuTheme::labelFont(type::caption));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(subtitle_, b.removeFromTop(16), juce::Justification::centredLeft, true);
        if (note_.isNotEmpty()) { // WF-12: the version note
            g.setColour(KhensuTheme::textSecondary);
            g.drawFittedText(note_, b.removeFromTop(30), juce::Justification::topLeft, 2);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) {
            if (onMenu_) onMenu_();
            return;
        }
        if (onClick_) onClick_();
    }
};

// ── RecentSessionsPanel ───────────────────────────────────────────────────────

// Every cached palette colour on this component, in one place (UI-01).
void RecentSessionsPanel::applyThemeColours()
{
    headerLabel.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
}

RecentSessionsPanel::RecentSessionsPanel()
{
    applyThemeColours();
    headerLabel.setText(TRANS("RECENT SESSIONS"), juce::dontSendNotification);
    headerLabel.setFont(khensu::KhensuTheme::uiFont(khensu::type::body, juce::Font::bold));
    addAndMakeVisible(headerLabel);

    scrollView_.setScrollBarsShown(false, true); // vertical off, horizontal on
    scrollView_.setScrollBarThickness(4);
    addAndMakeVisible(scrollView_);
}

void RecentSessionsPanel::setSessions(const std::vector<Session>& sessions)
{
    sessions_ = sessions;

    // Remove old cards from container
    cards_.clear();
    cardContainer_.removeAllChildren();

    // Build new card components; layoutCards() places them.
    for (size_t i = 0; i < sessions_.size(); ++i) {
        const Session& s = sessions_[i];
        std::string id = s.id;
        auto* card = cards_.add(new SessionCard(
            s,
            [this, id] {
                if (onSessionChosen) onSessionChosen(id);
            },
            [this, s] { showCardMenu(s); }));
        card->setTitle(juce::String(s.name));
        cardContainer_.addAndMakeVisible(card);
    }
    scrollView_.setViewedComponent(&cardContainer_, false);
    layoutCards();
    repaint();
}

void RecentSessionsPanel::layoutCards()
{
    static constexpr int kCardW = 160;
    // The horizontal scrollbar takes its strip under the cards.
    const int h = juce::jmax(0, scrollView_.getHeight() - scrollView_.getScrollBarThickness());
    int x = 0;
    for (auto* card : cards_) {
        card->setBounds(x, 0, kCardW, h);
        x += kCardW + KhensuTheme::cardGap;
    }
    const int totalW = (x > 0) ? (x - KhensuTheme::cardGap) : 0;
    cardContainer_.setSize(juce::jmax(totalW, scrollView_.getWidth()), h);
}

void RecentSessionsPanel::showCardMenu(const Session& s)
{
    juce::PopupMenu m;
    m.addItem(1, TRANS("Rename..."));
    m.addItem(2, TRANS("Version note..."));
    m.addItem(3, TRANS("Duplicate"));
    juce::Component::SafePointer<RecentSessionsPanel> self(this);
    m.showMenuAsync(juce::PopupMenu::Options(), [self, s](int choice) {
        if (self == nullptr) return;
        if (choice == 1 && self->onRenameSession) self->onRenameSession(s);
        if (choice == 2 && self->onEditSessionNote) self->onEditSessionNote(s);
        if (choice == 3 && self->onDuplicateSession) self->onDuplicateSession(s);
    });
}

void RecentSessionsPanel::setLoadError(bool failed)
{
    if (loadFailed_ == failed) return;
    loadFailed_ = failed;
    repaint();
}

void RecentSessionsPanel::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    g.setColour(KhensuTheme::bgSurface.withAlpha(0.25f));
    g.fillRoundedRectangle(bounds, 8.0f);

    juce::ColourGradient borderGrad(khensu::KhensuTheme::highlight.withAlpha(0.18f), bounds.getTopLeft(),
                                    KhensuTheme::bgVoid.withAlpha(0.60f), bounds.getBottomRight(), false);
    g.setGradientFill(borderGrad);
    g.drawRoundedRectangle(bounds, 8.0f, 1.2f);

    if (sessions_.empty()) {
        auto textArea = getLocalBounds().reduced(KhensuTheme::sp2, KhensuTheme::sp2);
        textArea.removeFromTop(24); // below the header label
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.setColour(loadFailed_ ? KhensuTheme::accentAmber : KhensuTheme::textTertiary);
        g.drawText(loadFailed_ ? TRANS("Couldn't load recent sessions - the session database may be locked or "
                                       "unavailable.")
                               : TRANS("No sessions yet. Drop a track above to start one."),
                   textArea, juce::Justification::centredLeft, true);
    }
}

void RecentSessionsPanel::resized()
{
    auto b = getLocalBounds().reduced(KhensuTheme::sp2, KhensuTheme::sp2);
    headerLabel.setBounds(b.removeFromTop(18));
    b.removeFromTop(KhensuTheme::sp1);
    scrollView_.setBounds(b);

    layoutCards();
}

} // namespace khensu
