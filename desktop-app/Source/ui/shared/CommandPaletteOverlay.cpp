// desktop-app/Source/ui/shared/CommandPaletteOverlay.cpp
#include "CommandPaletteOverlay.h"
#include "KhensuTheme.h"

namespace khensu {

// ---- inner search field ----------------------------------------------------
// A one-line TextEditor that hands navigation keys (Up/Down/Enter/Esc) to the
// palette before its own caret handling, so typing filters while the arrows
// drive the result list.
class CommandPaletteOverlay::SearchEditor final : public juce::TextEditor {
  public:
    std::function<bool(const juce::KeyPress&)> onNavKey;

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (onNavKey && onNavKey(key)) return true;
        return juce::TextEditor::keyPressed(key);
    }
};

// ---- fuzzy scoring ----------------------------------------------------------
int CommandPaletteOverlay::fuzzyScore(const juce::String& pattern, const juce::String& text)
{
    return palette_search::fuzzyScore(pattern, text);
}

// ---- ctor/dtor -------------------------------------------------------------
// Every cached palette colour on this overlay, in one place (UI-01). Guarded:
// the handler can fire before search_ is constructed.
void CommandPaletteOverlay::applyThemeColours()
{
    if (search_ == nullptr) return;
    search_->setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    search_->setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    search_->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    search_->setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
    search_->setColour(juce::TextEditor::highlightColourId, KhensuTheme::accentTeal.withAlpha(0.25f));
    search_->setColour(juce::CaretComponent::caretColourId, KhensuTheme::accentTeal);
    search_->setTextToShowWhenEmpty(TRANS("Search commands, cards, genres and findings..."),
                                    KhensuTheme::textTertiary);
}

CommandPaletteOverlay::CommandPaletteOverlay(std::vector<Command> commands) : commands_(std::move(commands))
{
    setWantsKeyboardFocus(true);
    setInterceptsMouseClicks(true, true);

    search_ = std::make_unique<SearchEditor>();
    search_->setFont(KhensuTheme::bodyFont(type::title));
    applyThemeColours();
    search_->setJustification(juce::Justification::centredLeft);
    search_->onTextChange = [this]() {
        selected_ = 0;
        firstVisible_ = 0;
        refilter();
        repaint();
    };
    search_->onNavKey = [this](const juce::KeyPress& key) -> bool {
        if (key == juce::KeyPress::escapeKey) {
            if (onDismiss) onDismiss();
            return true;
        }
        if (key == juce::KeyPress::upKey) {
            moveSelection(-1);
            return true;
        }
        if (key == juce::KeyPress::downKey) {
            moveSelection(+1);
            return true;
        }
        if (key == juce::KeyPress::returnKey) {
            executeSelected();
            return true;
        }
        return false;
    };
    addAndMakeVisible(*search_);

    refilter();
}

CommandPaletteOverlay::~CommandPaletteOverlay() = default;

void CommandPaletteOverlay::enableFreeTextCommand(std::function<void(const juce::String&)> handler)
{
    freeTextHandler_ = std::move(handler);
    if (freeTextIndex_ < 0) {
        commands_.push_back({TRANS("Do it:"), "Ask", "", nullptr});
        freeTextIndex_ = (int)commands_.size() - 1;
    }
    refilter();
    repaint();
}

void CommandPaletteOverlay::parentHierarchyChanged()
{
    if (isShowing() && search_ != nullptr && !search_->hasKeyboardFocus(false)) search_->grabKeyboardFocus();
}

// ---- filtering -------------------------------------------------------------
void CommandPaletteOverlay::refilter()
{
    const juce::String q = search_ ? search_->getText() : juce::String();

    // STU-7: commands, cards, findings and genres, ranked and grouped by
    // kind (PaletteSearch.h). The free-text row is placed below, never
    // fuzzy-matched.
    std::vector<palette_search::Entry> entries;
    entries.reserve(commands_.size());
    for (const auto& c : commands_)
        entries.push_back({c.name, c.keywords, c.kind});
    filtered_ = palette_search::rank(q, entries, freeTextIndex_);

    // CE-AG-4: a multi-word query can also be run as a mix instruction.
    const juce::String instruction = q.trim();
    if (freeTextIndex_ >= 0 && instruction.containsChar(' ')) {
        auto& cmd = commands_[(size_t)freeTextIndex_];
        cmd.name = TRANS("Do it:") + " \"" + instruction + "\"";
        cmd.action = [handler = freeTextHandler_, instruction] {
            if (handler) handler(instruction);
        };
        // Loose subsequence matching finds "commands" in most sentences, so a
        // query of three or more words is treated as an instruction first.
        const bool sentence = juce::StringArray::fromTokens(instruction, " ", "").size() >= 3;
        if (sentence)
            filtered_.insert(filtered_.begin(), freeTextIndex_);
        else
            filtered_.push_back(freeTextIndex_);
    }

    if (selected_ >= (int)filtered_.size()) selected_ = juce::jmax(0, (int)filtered_.size() - 1);
}

void CommandPaletteOverlay::moveSelection(int delta)
{
    if (filtered_.empty()) return;
    selected_ = juce::jlimit(0, (int)filtered_.size() - 1, selected_ + delta);
    const int rows = visibleRowCount();
    if (selected_ < firstVisible_)
        firstVisible_ = selected_;
    else if (selected_ >= firstVisible_ + rows)
        firstVisible_ = selected_ - rows + 1;
    repaint();
}

void CommandPaletteOverlay::executeSelected()
{
    if (selected_ < 0 || selected_ >= (int)filtered_.size()) return;
    auto action = commands_[filtered_[(size_t)selected_]].action;
    if (onDismiss) onDismiss(); // close first - action may replace this overlay
    if (action) action();
}

// ---- layout ----------------------------------------------------------------
int CommandPaletteOverlay::visibleRowCount() const { return juce::jmin(kMaxRows, (int)filtered_.size()); }

juce::Rectangle<int> CommandPaletteOverlay::cardBounds() const
{
    const int w = juce::jmin(kCardW, getWidth() - 80);
    const int rows = juce::jmax(1, visibleRowCount());
    const int h = kSearchH + kPad + rows * kRowH + kPad;
    const int x = (getWidth() - w) / 2;
    const int y = juce::jmax(40, getHeight() / 8);
    return {x, y, w, h};
}

void CommandPaletteOverlay::resized()
{
    auto card = cardBounds();
    search_->setBounds(card.removeFromTop(kSearchH).reduced(16, 6));
}

int CommandPaletteOverlay::rowIndexAt(juce::Point<int> p) const
{
    auto card = cardBounds();
    auto list = card.withTrimmedTop(kSearchH + kPad).reduced(kPad, 0);
    if (!list.contains(p)) return -1;
    const int row = (p.y - list.getY()) / kRowH;
    const int idx = firstVisible_ + row;
    return (idx >= 0 && idx < (int)filtered_.size()) ? idx : -1;
}

// ---- paint -----------------------------------------------------------------
void CommandPaletteOverlay::paint(juce::Graphics& g)
{
    g.fillAll(KhensuTheme::scrim); // themed modal scrim (token contract)

    auto card = cardBounds();
    g.setColour(KhensuTheme::bgSurface);
    g.fillRoundedRectangle(card.toFloat(), 10.0f);
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.35f));
    g.drawRoundedRectangle(card.toFloat().reduced(0.5f), 10.0f, 1.0f);

    // Search / list divider
    auto searchRow = card.removeFromTop(kSearchH);
    g.setColour(KhensuTheme::borderSubtle);
    g.fillRect(searchRow.getX() + 12, searchRow.getBottom() - 1, searchRow.getWidth() - 24, 1);

    // Magnifier hint glyph on the left of the search row (drawn under the editor's left inset)
    g.setColour(KhensuTheme::textTertiary);

    auto list = card.withTrimmedTop(kPad).reduced(kPad, 0);

    if (filtered_.empty()) {
        g.setFont(KhensuTheme::bodyFont(type::large));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(TRANS("No matches"), list.removeFromTop(kRowH), juce::Justification::centred, true);
        return;
    }

    const int rows = visibleRowCount();
    for (int r = 0; r < rows; ++r) {
        const int idx = firstVisible_ + r;
        if (idx >= (int)filtered_.size()) break;
        const auto& cmd = commands_[filtered_[(size_t)idx]];
        auto row = list.removeFromTop(kRowH);

        // STU-7: a hairline where one kind's group of results ends.
        if (idx > 0 && commands_[filtered_[(size_t)idx - 1]].kind != cmd.kind) {
            g.setColour(KhensuTheme::borderSubtle);
            g.fillRect(row.getX() + 12, row.getY(), row.getWidth() - 24, 1);
        }

        const bool sel = (idx == selected_);
        if (sel) {
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.14f));
            g.fillRoundedRectangle(row.toFloat().reduced(2.0f, 3.0f), 6.0f);
            g.setColour(KhensuTheme::accentTeal);
            g.fillRoundedRectangle((float)row.getX(), (float)row.getY() + 6.0f, 3.0f, (float)row.getHeight() - 12.0f,
                                   1.5f);
        }

        auto text = row.reduced(14, 0);
        // Category tag on the right
        auto tag = text.removeFromRight(64);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.setColour(sel ? KhensuTheme::accentTeal : KhensuTheme::textTertiary);
        g.drawText(cmd.category.toUpperCase(), tag, juce::Justification::centredRight, false);

        g.setFont(KhensuTheme::bodyFont(type::subtitle));
        g.setColour(sel ? KhensuTheme::textPrimary : KhensuTheme::textSecondary);
        g.drawText(cmd.name, text, juce::Justification::centredLeft, true);
    }
}

// ---- mouse -----------------------------------------------------------------
void CommandPaletteOverlay::mouseDown(const juce::MouseEvent& e)
{
    if (!cardBounds().contains(e.getPosition())) { // click outside dismisses
        if (onDismiss) onDismiss();
        return;
    }
    const int idx = rowIndexAt(e.getPosition());
    if (idx >= 0) {
        selected_ = idx;
        executeSelected();
    }
}

void CommandPaletteOverlay::mouseMove(const juce::MouseEvent& e)
{
    const int idx = rowIndexAt(e.getPosition());
    if (idx >= 0 && idx != selected_) {
        selected_ = idx;
        repaint();
    }
}

} // namespace khensu
