// desktop-app/Source/ui/Dashboard/CardGroup.h
// CON-2..CON-8: one Dashboard card for what used to be several - "Stereo &
// mono", "Tone", "Loudness & delivery", "Provenance & source", "Stems" - with
// the former cards as tabs. The member cards are unchanged (they still get
// their data, reset per session and run their scans); the group owns their
// placement: one shows, under a row of tabs. A member's id still finds the
// group (DashboardScreen::panelById), and showing a member selects its tab.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h"
#include <memory>
#include <string>
#include <vector>

namespace khensu {

class CardGroup final : public juce::Component {
  public:
    struct Member {
        std::string id;
        juce::String label;
        juce::Component* card = nullptr;
    };

    CardGroup(std::string id, juce::String title) : id_(std::move(id)), title_(std::move(title))
    {
        setTitle(title_);
        setDescription(TRANS("A card with tabs; each tab is one view of the same subject."));
    }

    const std::string& id() const noexcept { return id_; }
    const juce::String& title() const noexcept { return title_; }
    const std::vector<Member>& members() const noexcept { return members_; }

    /// Re-parents `card` into the group as a tab (call once per member).
    void addMember(const std::string& memberId, const juce::String& label, juce::Component& card)
    {
        if (auto* parent = card.getParentComponent()) parent->removeChildComponent(&card);
        members_.push_back({memberId, label, &card});
        auto button = std::make_unique<juce::TextButton>(label);
        button->setClickingTogglesState(false);
        button->setTitle(title_ + " - " + label);
        button->setTooltip(TRANS("Show") + " " + label);
        const int index = static_cast<int>(members_.size()) - 1;
        button->onClick = [this, index] { select(index); };
        addAndMakeVisible(*button);
        tabs_.push_back(std::move(button));
        addChildComponent(card);
        select(active_ < 0 ? 0 : active_);
    }

    bool hasMember(const std::string& memberId) const noexcept { return indexOf(memberId) >= 0; }

    /// Shows the member's tab; false when it is not a member.
    bool selectMember(const std::string& memberId)
    {
        const int i = indexOf(memberId);
        if (i < 0) return false;
        select(i);
        return true;
    }

    const std::string& activeMemberId() const
    {
        static const std::string none;
        return active_ >= 0 ? members_[static_cast<size_t>(active_)].id : none;
    }

    static constexpr int kTabRowH = 30;
    /// Room for the tallest member view under the tab row.
    int preferredHeight() const noexcept { return kTabRowH + 380; }

    void paint(juce::Graphics& g) override
    {
        auto tabRow = getLocalBounds().removeFromTop(kTabRowH).toFloat();
        g.setColour(KhensuTheme::bgSurface);
        g.fillRoundedRectangle(tabRow, 6.0f);
        g.setColour(KhensuTheme::textPrimary);
        g.setFont(KhensuTheme::labelFont(12.0f));
        g.drawText(title_, tabRow.reduced(10.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        auto row = b.removeFromTop(kTabRowH).reduced(4, 3);
        const int titleW = juce::GlyphArrangement::getStringWidthInt(KhensuTheme::labelFont(12.0f), title_) + 24;
        row.removeFromLeft(juce::jmin(titleW, row.getWidth() / 3));
        const int n = static_cast<int>(tabs_.size());
        if (n > 0) {
            const int w = juce::jmax(60, row.getWidth() / n);
            for (auto& t : tabs_)
                t->setBounds(row.removeFromLeft(w).reduced(2, 0));
        }
        b.removeFromTop(4);
        for (auto& m : members_)
            m.card->setBounds(b);
    }

  private:
    int indexOf(const std::string& memberId) const noexcept
    {
        for (size_t i = 0; i < members_.size(); ++i)
            if (members_[i].id == memberId) return static_cast<int>(i);
        return -1;
    }

    void select(int index)
    {
        if (index < 0 || index >= static_cast<int>(members_.size())) return;
        active_ = index;
        for (size_t i = 0; i < members_.size(); ++i) {
            const bool on = static_cast<int>(i) == index;
            members_[i].card->setVisible(on);
            tabs_[i]->setToggleState(on, juce::dontSendNotification);
        }
        applyThemeColours();
        resized();
    }

    void applyThemeColours()
    {
        for (size_t i = 0; i < tabs_.size(); ++i) {
            const bool on = static_cast<int>(i) == active_;
            tabs_[i]->setColour(juce::TextButton::buttonColourId,
                                on ? KhensuTheme::accentTeal.withAlpha(0.25f) : KhensuTheme::bgBase);
            tabs_[i]->setColour(juce::TextButton::textColourOffId,
                                on ? KhensuTheme::accentTeal : KhensuTheme::textSecondary);
        }
        repaint();
    }

    std::string id_;
    juce::String title_;
    std::vector<Member> members_;
    std::vector<std::unique_ptr<juce::TextButton>> tabs_;
    int active_ = -1;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] { applyThemeColours(); }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CardGroup)
};

} // namespace khensu
