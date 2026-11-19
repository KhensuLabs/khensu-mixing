// desktop-app/Source/ui/shared/PanelCard.cpp
#include "PanelCard.h"
#include "CardFindingRow.h"
#include "KhensuLookAndFeel.h"
#include "KhensuTheme.h"
#include "KhensuEmptyState.h"

namespace khensu {

void PanelCard::drawEmptyState(juce::Graphics& g, const juce::String& headline, const juce::String& body,
                               int topOffset) const
{
    auto area = bodyArea();
    if (topOffset > 0) area.removeFromTop(topOffset);
    KhensuEmptyState::paintInto(g, area, headline, body);
}

PanelCard::PanelCard(PanelCardController& controller, const juce::String& title) : controller_(controller), title_(title)
{
    // Screen readers announce the panel name when focus enters it, and
    // AccessibilityCoverage uses it to name otherwise-unlabelled controls.
    setTitle(title);
    setFocusContainerType(juce::Component::FocusContainerType::focusContainer);
    applyThemeColours();
    explainerButton_.onClick = [this] {
        controller_.showExplainer(title_, explCategory_, explDescription_, systemBrainFeatureId_);
    };
}

PanelCard::~PanelCard() = default;

void PanelCard::setFindingCodes(std::vector<std::string> codes)
{
    if (codes.empty()) {
        findingRow_.reset();
        resized();
        return;
    }
    findingRow_ = std::make_unique<CardFindingRow>(controller_, std::move(codes));
    addChildComponent(*findingRow_);
    // The body shrinks or grows with the row: lay the card out again.
    findingRow_->onShownOrHidden = [this] {
        resized();
        repaint();
    };
    findingRow_->refresh();
}

void PanelCard::applyThemeColours()
{
    explainerButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgSurface);
}

void PanelCard::setExplainer(const juce::String& category, const juce::String& description,
                             const juce::String& systemBrainFeatureId)
{
    explCategory_ = category;
    explDescription_ = description;
    systemBrainFeatureId_ = systemBrainFeatureId;
    if (!hasExplainer_) {
        hasExplainer_ = true;
        explainerButton_.setTitle(TRANS("Explain ") + title_);
        explainerButton_.setTooltip(TRANS("What this panel shows, and why it matters."));
        addAndMakeVisible(explainerButton_);
    }
}

void PanelCard::setAnalysisState(PanelAnalysisState state)
{
    if (analysisState_ == state) return;
    analysisState_ = state;
    setLoading(analysisState_ == PanelAnalysisState::Analyzing);
    repaint();
}

void PanelCard::setLoading(bool loading)
{
    if (isLoading_ == loading) return;
    isLoading_ = loading;
    if (isLoading_) {
        addChildComponent(skeleton_);
        // A collapsed card is header-only, so there is no body to shimmer in.
        // start() would setVisible(true) and paint the skeleton over the header
        // (kimi audit P1 #17); setCollapsed() starts it on expand instead.
        if (!collapsed_) skeleton_.start();
    } else {
        skeleton_.stop();
        removeChildComponent(&skeleton_);
    }
    resized();
    repaint();
}

void PanelCard::setConfidenceOverlayEnabled(bool enabled)
{
    if (confidenceOverlayEnabled_ == enabled) return;
    confidenceOverlayEnabled_ = enabled;
    repaint();
}

void PanelCard::setDraggable(const juce::String& dragId)
{
    draggable_ = true;
    dragId_ = dragId;
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
}

void PanelCard::setCollapsible(bool collapsible, std::function<void(bool)> onToggle)
{
    collapsible_ = collapsible;
    onCollapseToggle_ = std::move(onToggle);
    repaint();
}

void PanelCard::setCollapsed(bool collapsed)
{
    if (collapsed_ == collapsed) return;
    collapsed_ = collapsed;
    // Hide/show every child except the explainer so subclass content vanishes
    // when the card is collapsed to header-only. The owner resizes us to
    // collapsedHeight(); subclass layout resumes on expand.
    for (int i = 0; i < getNumChildComponents(); ++i) {
        auto* child = getChildComponent(i);
        if (isChromeChild(child)) continue;
        if (child == &resizeGrip_ && !resizeGrip_.onRequest) continue;
        child->setVisible(!collapsed_);
    }
    // Keep the shimmer consistent with BOTH flags: a card that collapses while
    // still loading stops animating (and stops burning a 30Hz timer), and one
    // that expands while still loading resumes.
    if (isLoading_) {
        if (collapsed_)
            skeleton_.stop();
        else
            skeleton_.start();
    }
    // The finding row's visibility is its own (open finding or not), not
    // the card's: re-evaluate it on expand.
    if (findingRow_ != nullptr) {
        if (collapsed_)
            findingRow_->setVisible(false);
        else
            findingRow_->refresh();
    }
    if (onCollapseToggle_) onCollapseToggle_(collapsed_);
    repaint();
}

void PanelCard::childrenChanged()
{
    if (!collapsed_) return;
    for (int i = 0; i < getNumChildComponents(); ++i) {
        auto* child = getChildComponent(i);
        if (!isChromeChild(child)) child->setVisible(false);
    }
}

void PanelCard::setPopOutHandler(std::function<void()> handler)
{
    popOutHandler_ = std::move(handler);
    popOutButton_.setTooltip(TRANS("Pop this panel out into its own resizable window."));
    popOutButton_.setTitle(TRANS("Pop out ") + title_);
    popOutButton_.setWantsKeyboardFocus(true);
    popOutButton_.onClick = [this] {
        if (popOutHandler_) popOutHandler_();
    };
    addAndMakeVisible(popOutButton_);
    resized();
}

void PanelCard::setFloating(bool floating)
{
    floating_ = floating;
    popOutButton_.docked = !floating;
    popOutButton_.setTitle((floating ? "Dock " : "Pop out ") + title_);
    popOutButton_.setTooltip(floating ? "Dock this panel back into its screen."
                                      : "Pop this panel out into its own resizable window.");
    resizeGrip_.setVisible(!floating && resizeGrip_.onRequest != nullptr && !collapsed_);
    popOutButton_.repaint();
}

void PanelCard::setHeightResizeHandler(std::function<void(int)> handler)
{
    resizeGrip_.onRequest = std::move(handler);
    resizeGrip_.setTitle(TRANS("Resize ") + title_);
    resizeGrip_.setDescription(TRANS("Drag, or press Up/Down, to change this panel's height. Delete resets it."));
    resizeGrip_.setWantsKeyboardFocus(true);
    resizeGrip_.setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
    resizeGrip_.setTooltip(TRANS("Drag to resize this panel (double-click resets)."));
    addAndMakeVisible(resizeGrip_);
    resized();
}

void PanelCard::PopOutButton::paintButton(juce::Graphics& g, bool over, bool down)
{
    auto r = getLocalBounds().toFloat().reduced(4.0f);
    g.setColour((over || down ? KhensuTheme::accentTeal : KhensuTheme::textTertiary));
    g.drawRoundedRectangle(r.withTrimmedRight(4.0f).withTrimmedTop(4.0f), 2.0f, 1.2f);
    // Arrow from the box's centre toward its top-right corner (pop out), or
    // the reverse (dock back).
    const auto c = r.getCentre();
    const auto corner = r.getTopRight();
    const auto from = docked ? c : corner;
    const auto to = docked ? corner : c;
    g.drawArrow(juce::Line<float>(from, to), 1.4f, 5.0f, 5.0f);
    if (hasKeyboardFocus(false)) {
        g.setColour(KhensuTheme::focusRing);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 3.0f, 1.0f);
    }
}

void PanelCard::ResizeGrip::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour((isMouseOverOrDragging() || hasKeyboardFocus(false) ? KhensuTheme::accentTeal
                                                                      : KhensuTheme::borderSubtle)
                    .withAlpha(0.8f));
    const float cx = r.getCentreX();
    g.fillRoundedRectangle(cx - 18.0f, r.getCentreY() - 1.0f, 36.0f, 2.0f, 1.0f);
}

void PanelCard::ResizeGrip::mouseDown(const juce::MouseEvent&)
{
    startHeight = getParentComponent() != nullptr ? getParentComponent()->getHeight() : 0;
}

void PanelCard::ResizeGrip::mouseDrag(const juce::MouseEvent& e)
{
    if (onRequest) onRequest(startHeight + e.getDistanceFromDragStartY());
}

void PanelCard::ResizeGrip::mouseDoubleClick(const juce::MouseEvent&)
{
    if (onRequest) onRequest(-1);
}

bool PanelCard::ResizeGrip::keyPressed(const juce::KeyPress& key)
{
    if (!onRequest || getParentComponent() == nullptr) return false;
    const int h = getParentComponent()->getHeight();
    if (key == juce::KeyPress::upKey) onRequest(h - 20);
    else if (key == juce::KeyPress::downKey) onRequest(h + 20);
    else if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) onRequest(-1);
    else return false;
    return true;
}

juce::Rectangle<int> PanelCard::chevronBounds() const
{
    const int x = getWidth() - (hasExplainer_ ? 30 : 8) - 20;
    return {x, 4, 18, 22};
}

void PanelCard::mouseDown(const juce::MouseEvent& e)
{
    if (collapsible_ && chevronBounds().contains(e.getPosition())) {
        setCollapsed(!collapsed_);
        return;
    }
    dragArmed_ = draggable_ && e.y < kHeaderH;
}

void PanelCard::mouseDrag(const juce::MouseEvent& e)
{
    if (!dragArmed_) return;
    if (e.getDistanceFromDragStart() < 6) return; // small deadzone before committing to a drag

    dragArmed_ = false;
    if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this))
        container->startDragging(dragId_, this);
}

juce::Rectangle<int> PanelCard::bodyArea() const
{
    auto area = getLocalBounds().withTrimmedTop(kHeaderH).reduced(12, 8);
    if (findingRow_ != nullptr && findingRow_->isVisible() && !collapsed_)
        area.removeFromBottom(CardFindingRow::kHeight + 4); // UX-1 row
    // A collapsed (or simply very short) card leaves nothing below the header,
    // and Rectangle::reduced() will happily return a NEGATIVE size there, which
    // then propagates into skeleton_/empty-state bounds (kimi audit P1 #17).
    if (area.getWidth() <= 0 || area.getHeight() <= 0) return {area.getX(), area.getY(), 0, 0};
    return area;
}

void PanelCard::paint(juce::Graphics& g)
{
    // Backdrop blur: this card's region of the app-wide blurred background,
    // in the root's coordinate space (the space drawAppBackground() uses),
    // independent of any intermediate viewport's scroll offset.
    auto* root = getTopLevelComponent();
    if (root != nullptr && root != this)
        KhensuLookAndFeel::drawGlassPanel(g, getLocalBounds(), GlassPanelVariant::Default,
                                          KhensuLookAndFeel::glassBackdrop(root->getLocalBounds(), GlassPanelVariant::Default),
                                          root->getLocalArea(this, getLocalBounds()));
    else // not parented into a real window yet
        KhensuLookAndFeel::drawGlassPanel(g, getLocalBounds(), GlassPanelVariant::Default);

    // Section header label
    auto headerBounds = getLocalBounds().removeFromTop(kHeaderH).reduced(KhensuTheme::sp4, 0);

    if (analysisState_ != PanelAnalysisState::NotApplicable) {
        auto chipArea = headerBounds.removeFromRight(hasExplainer_ ? 106 : 90).reduced(0, 8);

        juce::Colour dotColour;
        juce::String label;
        switch (analysisState_) {
        case PanelAnalysisState::Analyzed:
            dotColour = KhensuTheme::accentTeal;
            label = "ANALYZED";
            break;
        case PanelAnalysisState::Analyzing:
            dotColour = KhensuTheme::accentAmber;
            label = "ANALYZING...";
            break;
        case PanelAnalysisState::NotAnalyzed:
            dotColour = KhensuTheme::textTertiary;
            label = "NOT ANALYZED";
            break;
        default:
            break;
        }
        auto dotArea = chipArea.removeFromLeft(8).withSizeKeepingCentre(6, 6);
        g.setColour(dotColour);
        g.fillEllipse(dotArea.toFloat());
        chipArea.removeFromLeft(4);
        g.setFont(KhensuTheme::labelFont(9.0f));
        g.setColour(dotColour);
        g.drawText(label, chipArea, juce::Justification::centredLeft, true);
    }

    if (draggable_) {
        auto gripArea = headerBounds.removeFromLeft(14);
        g.setColour(KhensuTheme::textTertiary.withAlpha(0.5f));
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 2; ++col)
                g.fillEllipse((float)gripArea.getX() + (float)col * 5.0f,
                              (float)gripArea.getCentreY() - 6.0f + (float)row * 5.0f, 2.0f, 2.0f);
        headerBounds.removeFromLeft(2);
    }

    // Title case, not ALL CAPS (typography pass): titles are stored Title Case
    // ("Reference Compare"); a semibold header reads as a section without
    // shouting. Weight carries the hierarchy instead of caps.
    g.setFont(KhensuTheme::sectionHeaderFont().boldened());
    g.setColour(KhensuTheme::textSecondary);
    g.drawText(title_, headerBounds, juce::Justification::centredLeft, true);

    // Collapse chevron (opt-in): points right when collapsed, down when open.
    if (collapsible_) {
        auto cb = chevronBounds().toFloat();
        const float cx = cb.getCentreX(), cy = cb.getCentreY();
        juce::Path chevron;
        if (collapsed_) {
            chevron.addTriangle(cx - 3.0f, cy - 4.0f, cx - 3.0f, cy + 4.0f, cx + 3.0f, cy);
        } else {
            chevron.addTriangle(cx - 4.0f, cy - 3.0f, cx + 4.0f, cy - 3.0f, cx, cy + 3.0f);
        }
        g.setColour(KhensuTheme::textSecondary);
        g.fillPath(chevron);
    }

    // Header rule
    int ruleY = kHeaderH - 1;
    g.setColour(KhensuTheme::borderSubtle);
    g.drawHorizontalLine(ruleY, (float)KhensuTheme::sp4, (float)(getWidth() - KhensuTheme::sp4));

    if (collapsed_) return; // header-only when collapsed

    // Confidence & Completeness Overlay: dim + diagonal-hatch panels that
    // aren't confidently Analyzed, so incomplete data doesn't read as a
    // confident answer while the user is scanning the Dashboard.
    if (confidenceOverlayEnabled_ &&
        (analysisState_ == PanelAnalysisState::NotAnalyzed || analysisState_ == PanelAnalysisState::Analyzing)) {
        auto body = bodyArea();
        g.setColour(KhensuTheme::bgVoid.withAlpha(0.55f));
        g.fillRect(body);

        g.saveState();
        g.reduceClipRegion(body);
        g.setColour(KhensuTheme::textTertiary.withAlpha(0.18f));
        const int step = 14;
        for (int x = -body.getHeight(); x < body.getWidth(); x += step)
            g.drawLine((float)(body.getX() + x), (float)body.getBottom(), (float)(body.getX() + x + body.getHeight()),
                       (float)body.getY(), 1.5f);
        g.restoreState();
    }
}

void PanelCard::setExperimental(bool experimental, const juce::String& outstandingCheck)
{
    experimental_ = experimental;
    if (experimental) {
        experimentalBadge_.setOutstandingCheck(outstandingCheck);
        if (experimentalBadge_.getParentComponent() == nullptr) addChildComponent(experimentalBadge_);
    }
    experimentalBadge_.setVisible(experimental);
    resized();
    repaint();
}

void PanelCard::resized()
{
    if (experimental_) {
        // Beside the title, measured with the font the title is drawn in.
        int x = KhensuTheme::sp4 + (draggable_ ? 16 : 0) +
                juce::GlyphArrangement::getStringWidthInt(KhensuTheme::sectionHeaderFont().boldened(), title_) + 8;
        const int w = ExperimentalBadge::preferredWidth();
        x = juce::jmax(KhensuTheme::sp4, juce::jmin(x, getWidth() - w - 140));
        experimentalBadge_.setBounds(x, (kHeaderH - ExperimentalBadge::kHeight) / 2, w, ExperimentalBadge::kHeight);
    }
    if (hasExplainer_) explainerButton_.setBounds(getWidth() - 30, 4, 22, 22);
    if (popOutHandler_) {
        const int x = getWidth() - (hasExplainer_ ? 30 : 8) - (collapsible_ ? 22 : 0) - 24;
        popOutButton_.setBounds(x, 5, 22, 20);
    }
    if (resizeGrip_.onRequest) resizeGrip_.setBounds(0, getHeight() - 8, getWidth(), 8);
    if (isLoading_) skeleton_.setBounds(bodyArea());
    if (findingRow_ != nullptr)
        findingRow_->setBounds(getLocalBounds()
                                   .withTrimmedTop(kHeaderH)
                                   .reduced(12, 8)
                                   .removeFromBottom(CardFindingRow::kHeight));
}

} // namespace khensu
