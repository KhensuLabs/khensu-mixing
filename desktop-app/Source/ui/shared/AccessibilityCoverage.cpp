// desktop-app/Source/ui/shared/AccessibilityCoverage.cpp
#include "AccessibilityCoverage.h"

namespace khensu::AccessibilityCoverage {

namespace {
bool hasLetters(const juce::String& s)
{
    for (auto c : s)
        if (juce::CharacterFunctions::isLetter(c)) return true;
    return false;
}

/// A Label that names `c`: attached via attachToComponent(), or a sibling
/// label whose component name matches (JUCE convention for "label for").
const juce::Label* labelFor(const juce::Component& c)
{
    auto* parent = c.getParentComponent();
    if (parent == nullptr) return nullptr;
    for (auto* child : parent->getChildren())
        if (auto* label = dynamic_cast<const juce::Label*>(child))
            if (label->getAttachedComponent() == &c) return label;
    // The house pattern is addAndMakeVisible(label); addAndMakeVisible(control):
    // a non-editable label immediately before a slider/combo/editor names it.
    if (dynamic_cast<const juce::Button*>(&c) != nullptr) return nullptr;
    const int index = parent->getIndexOfChildComponent(&c);
    if (index > 0)
        if (auto* label = dynamic_cast<const juce::Label*>(parent->getChildComponent(index - 1)))
            if (!label->isEditable() && label->getAttachedComponent() == nullptr) return label;
    // Once laid out: the plain label immediately to the LEFT on the same row,
    // or directly ABOVE and horizontally overlapping (the two caption
    // positions this UI uses).
    const auto cb = c.getBounds();
    if (cb.isEmpty()) return nullptr;
    const juce::Label* best = nullptr;
    int bestDistance = 1 << 30;
    for (auto* child : parent->getChildren()) {
        auto* label = dynamic_cast<const juce::Label*>(child);
        if (label == nullptr || label->isEditable() || label->getText().trim().isEmpty()) continue;
        const auto lb = label->getBounds();
        int distance = 1 << 30;
        const bool sameRow = lb.getY() < cb.getBottom() && lb.getBottom() > cb.getY();
        if (sameRow && lb.getRight() <= cb.getX() + 2) distance = cb.getX() - lb.getRight();
        const bool above = lb.getBottom() <= cb.getY() + 2 && lb.getX() < cb.getRight() && lb.getRight() > cb.getX();
        if (above) distance = juce::jmin(distance, cb.getY() - lb.getBottom());
        if (distance < 40 && distance < bestDistance) {
            bestDistance = distance;
            best = label;
        }
    }
    return best;
}

juce::String tooltipOf(const juce::Component& c)
{
    if (auto* t = dynamic_cast<const juce::TooltipClient*>(&c)) {
        // getTooltip() is non-const on TooltipClient.
        return const_cast<juce::TooltipClient*>(t)->getTooltip();
    }
    return {};
}

juce::String derivedName(const juce::Component& c)
{
    if (auto* b = dynamic_cast<const juce::Button*>(&c)) {
        const auto text = b->getButtonText().trim();
        if (hasLetters(text)) return text;
        if (auto words = wordsForSymbol(text); words.isNotEmpty()) {
            const auto tip = tooltipOf(c);
            return tip.isNotEmpty() ? tip.upToFirstOccurrenceOf(".", false, false) : words;
        }
    }
    if (auto* label = labelFor(c)) {
        const auto t = label->getText().trim().trimCharactersAtEnd(":");
        if (t.isNotEmpty()) return t;
    }
    if (auto* combo = dynamic_cast<const juce::ComboBox*>(&c)) {
        if (combo->getTextWhenNothingSelected().isNotEmpty()) return combo->getTextWhenNothingSelected();
    }
    if (auto* editor = dynamic_cast<const juce::TextEditor*>(&c)) {
        if (editor->getTextToShowWhenEmpty().isNotEmpty()) return editor->getTextToShowWhenEmpty();
    }
    const auto name = c.getName().trim();
    if (hasLetters(name)) return name;
    const auto tip = tooltipOf(c);
    if (tip.isNotEmpty()) return tip.upToFirstOccurrenceOf(".", false, false);
    // Last resort: the nearest titled ancestor (every PanelCard exposes its
    // title) plus what kind of control this is - "Chord Progression text".
    for (auto* p = c.getParentComponent(); p != nullptr; p = p->getParentComponent()) {
        const auto owner = p->getTitle().trim();
        if (owner.isEmpty()) continue;
        if (auto* editor = dynamic_cast<const juce::TextEditor*>(&c))
            return owner + (editor->isReadOnly() ? " text" : " input");
        if (dynamic_cast<const juce::Slider*>(&c) != nullptr) return owner + " control";
        if (dynamic_cast<const juce::ComboBox*>(&c) != nullptr) return owner + " choice";
        if (dynamic_cast<const juce::ListBox*>(&c) != nullptr) return owner + " list";
        return {};
    }
    return {};
}

void walk(juce::Component& c, const std::function<void(juce::Component&)>& fn)
{
    fn(c);
    for (auto* child : c.getChildren())
        walk(*child, fn);
}

/// Internal pieces of composite JUCE widgets (a slider's value box, a
/// combo's label, scrollbar buttons) are named by their owner.
bool isInternalPart(const juce::Component& c)
{
    auto* parent = c.getParentComponent();
    if (parent == nullptr) return false;
    if (dynamic_cast<const juce::Slider*>(parent) != nullptr) return true;
    if (dynamic_cast<const juce::ComboBox*>(parent) != nullptr) return true;
    if (dynamic_cast<const juce::ScrollBar*>(parent) != nullptr) return true;
    if (dynamic_cast<const juce::ScrollBar*>(&c) != nullptr) return true;
    return false;
}
} // namespace

juce::String Gap::describe() const
{
    juce::String s = component != nullptr ? juce::String(typeid(*component).name()) : juce::String("null");
    if (component != nullptr) {
        if (auto* b = dynamic_cast<juce::Button*>(component)) s << " \"" << b->getButtonText() << "\"";
        s << " title=\"" << component->getTitle() << "\"";
    }
    if (component != nullptr) {
        // Owner chain (nearest first) so a report line can be traced to code.
        juce::StringArray chain;
        for (auto* p = component->getParentComponent(); p != nullptr && chain.size() < 3; p = p->getParentComponent())
            chain.add(juce::String(typeid(*p).name()).fromLastOccurrenceOf("::", false, false));
        s << " in " << chain.joinIntoString(" < ");
    }
    if (missingName) s << " [no name]";
    if (missingTooltip) s << " [no tooltip]";
    return s;
}

juce::String wordsForSymbol(const juce::String& text)
{
    const auto t = text.trim();
    if (t == "+") return "Increase";
    if (t == "-") return "Decrease";
    if (t == "?") return "Explain";
    if (t == "x" || t == "X") return "Close";
    if (t == "<" || t == "<<") return "Previous";
    if (t == ">" || t == ">>") return "Next";
    if (t == "...") return "More options";
    if (t == "^") return "Move up";
    if (t == "v") return "Move down";
    return {};
}

bool isInteractive(const juce::Component& c)
{
    return dynamic_cast<const juce::Button*>(&c) != nullptr || dynamic_cast<const juce::Slider*>(&c) != nullptr ||
           dynamic_cast<const juce::ComboBox*>(&c) != nullptr || dynamic_cast<const juce::TextEditor*>(&c) != nullptr ||
           dynamic_cast<const juce::ListBox*>(&c) != nullptr;
}

int apply(juce::Component& root)
{
    int filled = 0;
    walk(root, [&filled](juce::Component& c) {
        if (!isInteractive(c) || isInternalPart(c)) return;
        if (c.getTitle().trim().isEmpty()) {
            const auto name = derivedName(c);
            if (name.isNotEmpty()) {
                c.setTitle(name);
                ++filled;
            }
        }
        if (auto* tip = dynamic_cast<juce::SettableTooltipClient*>(&c); tip != nullptr && tip->getTooltip().isEmpty()) {
            juce::String text = c.getDescription().isNotEmpty() ? c.getDescription() : c.getTitle();
            if (text.isNotEmpty()) {
                tip->setTooltip(text);
                ++filled;
            }
        }
    });
    return filled;
}

std::vector<Gap> audit(juce::Component& root)
{
    std::vector<Gap> gaps;
    walk(root, [&gaps](juce::Component& c) {
        if (!isInteractive(c) || isInternalPart(c) || !c.isVisible()) return;
        Gap g;
        g.component = &c;
        g.missingName = c.getTitle().trim().isEmpty() && derivedName(c).isEmpty();
        auto* tip = dynamic_cast<juce::SettableTooltipClient*>(&c);
        g.missingTooltip = tip != nullptr && tip->getTooltip().trim().isEmpty();
        if (g.missingName || g.missingTooltip) gaps.push_back(g);
    });
    return gaps;
}

} // namespace khensu::AccessibilityCoverage
