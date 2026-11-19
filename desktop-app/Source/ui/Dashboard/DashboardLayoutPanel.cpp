// desktop-app/Source/ui/Dashboard/DashboardLayoutPanel.cpp
#include "DashboardLayoutPanel.h"
#include "../shared/KhensuLookAndFeel.h"

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void DashboardLayoutPanel::applyThemeColours()
{
    titleLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
}

DashboardLayoutPanel::DashboardLayoutPanel(const std::vector<std::pair<std::string, juce::String>>& idsAndLabels,
                                           const std::map<std::string, bool>& initialVisibility,
                                           const juce::String& initialReportMode,
                                           std::map<juce::String, WorkspaceLayout> builtInPresets,
                                           std::vector<juce::String> savedPresetNames)
    : visibility_(initialVisibility), reportModeStr_(initialReportMode), builtInPresets_(std::move(builtInPresets))
{
    applyThemeColours();
    for (const auto& [id, label] : idsAndLabels) {
        order_.push_back(id);
        idToLabel_[id] = label;
    }

    addAndMakeVisible(card_);

    titleLabel_.setText(TRANS("Customize Dashboard Layout"), juce::dontSendNotification);
    titleLabel_.setFont(khensu::KhensuTheme::uiFont(15.0f, juce::Font::bold));
    card_.addAndMakeVisible(titleLabel_);

    closeButton_.onClick = [this]() {
        if (onClose) onClose();
    };
    card_.addAndMakeVisible(closeButton_);

    presetBox_.addItem("-- Load a preset --", 1);
    int itemId = 2;
    presetBox_.addSectionHeading("User Profiles / Workspace Modes");
    for (const auto& [name, layout] : builtInPresets_) {
        presetBox_.addItem(name, itemId++);
    }
    if (!savedPresetNames.empty()) {
        presetBox_.addSectionHeading("Saved Custom Layouts");
        for (const auto& name : savedPresetNames)
            presetBox_.addItem(name, itemId++);
    }
    presetBox_.setSelectedId(1, juce::dontSendNotification);
    presetBox_.onChange = [this]() {
        const auto name = presetBox_.getText();
        auto it = builtInPresets_.find(name);
        if (it != builtInPresets_.end())
            applyLayout(it->second);
        else if (onRequestSavedLayout)
            onRequestSavedLayout(name);
    };
    card_.addAndMakeVisible(presetBox_);

    saveNameEditor_.setTextToShowWhenEmpty(TRANS("Layout name..."), KhensuTheme::textSecondary);
    card_.addAndMakeVisible(saveNameEditor_);

    saveButton_.onClick = [this]() {
        auto name = saveNameEditor_.getText().trim();
        if (name.isEmpty()) return;
        if (onSaveAs) onSaveAs(name, currentLayout());
    };
    card_.addAndMakeVisible(saveButton_);

    reportModeToggle_.setToggleState(reportModeStr_ == "engineer", juce::dontSendNotification);
    reportModeToggle_.onClick = [this]() {
        reportModeStr_ = reportModeToggle_.getToggleState() ? "engineer" : "client";
        notifyApply();
    };
    card_.addAndMakeVisible(reportModeToggle_);

    viewport_.setViewedComponent(&rowsContainer_, false);
    viewport_.setScrollBarsShown(true, false);
    card_.addAndMakeVisible(viewport_);

    rebuildRows();

    grabModalFocus();
}

void DashboardLayoutPanel::applyLayout(const WorkspaceLayout& layout)
{
    // Start from the preset's order, then append any known ids it didn't
    // mention (keeps newly-added panels visible instead of disappearing).
    std::vector<std::string> newOrder = layout.panelOrder;
    for (const auto& id : order_)
        if (std::find(newOrder.begin(), newOrder.end(), id) == newOrder.end()) newOrder.push_back(id);
    // Drop any stale ids the preset mentions that no longer exist.
    newOrder.erase(std::remove_if(newOrder.begin(), newOrder.end(),
                                  [this](const std::string& id) { return idToLabel_.find(id) == idToLabel_.end(); }),
                   newOrder.end());

    order_ = std::move(newOrder);
    visibility_.clear();
    for (const auto& id : order_)
        visibility_[id] = layout.panelVisibility.count(id) ? layout.panelVisibility.at(id) : true;
    reportModeStr_ = layout.reportMode;
    reportModeToggle_.setToggleState(reportModeStr_ == "engineer", juce::dontSendNotification);

    rebuildRows();
    notifyApply();
}

WorkspaceLayout DashboardLayoutPanel::currentLayout() const
{
    WorkspaceLayout layout;
    layout.panelOrder = order_;
    layout.panelVisibility = visibility_;
    layout.reportMode = reportModeStr_;
    return layout;
}

void DashboardLayoutPanel::notifyApply()
{
    if (onApply) onApply(currentLayout());
}

void DashboardLayoutPanel::moveRow(int index, int delta)
{
    const int target = index + delta;
    if (target < 0 || target >= (int)order_.size()) return;
    std::swap(order_[(size_t)index], order_[(size_t)target]);
    rebuildRows();
    notifyApply();
}

void DashboardLayoutPanel::rebuildRows()
{
    for (auto& r : rows_)
        rowsContainer_.removeChildComponent(&r->label);
    rows_.clear();

    for (size_t i = 0; i < order_.size(); ++i) {
        const auto& id = order_[i];
        auto row = std::make_unique<Row>();
        row->id = id;
        row->label.setText(idToLabel_[id], juce::dontSendNotification);
        row->label.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
        rowsContainer_.addAndMakeVisible(row->label);

        row->visibleToggle.setToggleState(visibility_.count(id) ? visibility_[id] : true, juce::dontSendNotification);
        row->visibleToggle.onClick = [this, id]() {
            for (auto& r : rows_)
                if (r->id == id) {
                    visibility_[id] = r->visibleToggle.getToggleState();
                    break;
                }
            notifyApply();
        };
        rowsContainer_.addAndMakeVisible(row->visibleToggle);

        const int idx = (int)i;
        row->upButton.setEnabled(idx > 0);
        row->upButton.onClick = [this, idx]() { moveRow(idx, -1); };
        rowsContainer_.addAndMakeVisible(row->upButton);

        row->downButton.setEnabled(idx < (int)order_.size() - 1);
        row->downButton.onClick = [this, idx]() { moveRow(idx, 1); };
        rowsContainer_.addAndMakeVisible(row->downButton);

        rows_.push_back(std::move(row));
    }
    resized();
}

void DashboardLayoutPanel::resized()
{
    auto bounds = getLocalBounds();
    auto cardW = juce::jmin(520, bounds.getWidth() - 40);
    auto cardH = juce::jmin(600, bounds.getHeight() - 40);
    card_.setBounds((bounds.getWidth() - cardW) / 2, (bounds.getHeight() - cardH) / 2, cardW, cardH);

    auto b = card_.getLocalBounds().reduced(16);
    auto header = b.removeFromTop(24);
    closeButton_.setBounds(header.removeFromRight(70));
    titleLabel_.setBounds(header);
    b.removeFromTop(8);

    presetBox_.setBounds(b.removeFromTop(26));
    b.removeFromTop(6);
    reportModeToggle_.setBounds(b.removeFromTop(22));
    b.removeFromTop(10);

    auto saveRow = b.removeFromBottom(28);
    saveButton_.setBounds(saveRow.removeFromRight(120));
    saveRow.removeFromRight(6);
    saveNameEditor_.setBounds(saveRow);
    b.removeFromBottom(8);

    viewport_.setBounds(b);
    const int rowH = 30;
    const int w = viewport_.getWidth() - viewport_.getScrollBarThickness();
    int y = 0;
    for (auto& row : rows_) {
        auto rowBounds = juce::Rectangle<int>(0, y, w, rowH);
        row->visibleToggle.setBounds(rowBounds.removeFromLeft(28));
        row->downButton.setBounds(rowBounds.removeFromRight(28));
        row->upButton.setBounds(rowBounds.removeFromRight(28));
        row->label.setBounds(rowBounds);
        y += rowH;
    }
    rowsContainer_.setSize(w, y);
}

} // namespace khensu
