// plugin-copilot/Source/ui/SystemBrainMiniPanel.cpp
#include "SystemBrainMiniPanel.h"
#include <khensu/ui/KhensuPalette.h>
#include "PluginLookAndFeel.h"
#include <khensu/common/AppPaths.h>

namespace khensu {

// ---- FeatureMiniRow ----

void FeatureMiniRow::setEntry(const FeatureEntry& e)
{
    name_ = juce::String(e.name);
    category_ = juce::String(e.category).toUpperCase();
    shortDesc_ = juce::String(e.short_description);
    pluginAvailable_ = e.plugin_available;
    repaint();
}

void FeatureMiniRow::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().reduced(8, 4);

    // Plugin badge
    if (pluginAvailable_) {
        auto badge = b.removeFromRight(40);
        g.setColour(PluginLookAndFeel::statusGood.withAlpha(0.18f));
        g.fillRoundedRectangle(badge.toFloat().reduced(2, 6), 3.0f);
        g.setColour(PluginLookAndFeel::statusGood);
        g.setFont(khensu::ui::Palette::uiFont(8.0f, juce::Font::plain));
        g.drawText("PLUGIN", badge, juce::Justification::centred);
    }

    // Category chip
    auto nameRow = b.removeFromTop(16);
    auto chip = nameRow.removeFromLeft(juce::jmin(70, nameRow.getWidth()));
    g.setColour(PluginLookAndFeel::accentBlue.withAlpha(0.16f));
    g.fillRoundedRectangle(chip.toFloat(), 3.0f);
    g.setColour(PluginLookAndFeel::accentBlue);
    g.setFont(khensu::ui::Palette::uiFont(8.0f, juce::Font::plain));
    g.drawText(category_, chip, juce::Justification::centred);
    nameRow.removeFromLeft(6);

    // Name
    g.setColour(PluginLookAndFeel::textPrimary);
    g.setFont(khensu::ui::Palette::uiFont(11.0f, juce::Font::bold));
    g.drawText(name_, nameRow, juce::Justification::centredLeft, true);

    b.removeFromTop(2);

    // Short description
    g.setColour(PluginLookAndFeel::textMuted);
    g.setFont(khensu::ui::Palette::uiFont(10.0f, juce::Font::plain));
    g.drawText(shortDesc_, b, juce::Justification::centredLeft, true);

    // Separator
    g.setColour(PluginLookAndFeel::controlOutline);
    g.drawHorizontalLine(getHeight() - 1, 0.0f, (float)getWidth());
}

// ---- SystemBrainMiniPanel ----

SystemBrainMiniPanel::SystemBrainMiniPanel(BackgroundRunner runner)
{
    searchBox_.setTextToShowWhenEmpty("Search features...", PluginLookAndFeel::textMuted.withAlpha(0.7f));
    searchBox_.setFont(khensu::ui::Palette::uiFont(11.0f, juce::Font::plain));
    searchBox_.setColour(juce::TextEditor::backgroundColourId, PluginLookAndFeel::bgPanel);
    searchBox_.setColour(juce::TextEditor::textColourId, PluginLookAndFeel::textPrimary);
    searchBox_.setColour(juce::TextEditor::outlineColourId, PluginLookAndFeel::controlOutline);
    searchBox_.setColour(juce::TextEditor::focusedOutlineColourId, PluginLookAndFeel::accentTeal);
    searchBox_.onTextChange = [this]() { onSearchChanged(); };
    addAndMakeVisible(searchBox_);

    viewport_.setScrollBarsShown(true, false);
    viewport_.setViewedComponent(&listContent_, false);
    viewport_.setTitle("System Brain topics");
    addAndMakeVisible(viewport_);

    loadAsync(runner);
}

SystemBrainMiniPanel::~SystemBrainMiniPanel() {}

juce::File SystemBrainMiniPanel::resolveEncyclopediaFile(const juce::File& moduleFile)
{
    // 1. Inside the bundle: <bundle>/Contents/<arch>/<binary> on Windows and
    //    Linux, <bundle>/Contents/MacOS/<binary> on macOS.
    const auto contents = moduleFile.getParentDirectory().getParentDirectory();
    if (contents.getFileName() == "Contents") {
        const auto bundled = contents.getChildFile("Resources").getChildFile(kEncyclopediaFileName);
        if (bundled.existsAsFile()) return bundled;
    }
    // 2. A source/build tree (developer builds run straight from build/).
    for (auto dir = moduleFile.getParentDirectory(); dir.exists();) {
        const auto inTree = dir.getChildFile("resources/system_brain").getChildFile(kEncyclopediaFileName);
        if (inTree.existsAsFile()) return inTree;
        const auto parent = dir.getParentDirectory();
        if (parent == dir) break;
        dir = parent;
    }
    // 3. The desktop install's data directory.
    const auto installed = juce::File(juce::String(khensu::AppPaths::dataDirectory()))
                               .getChildFile("resources/system_brain")
                               .getChildFile(kEncyclopediaFileName);
    if (installed.existsAsFile()) return installed;
    return {};
}

void SystemBrainMiniPanel::loadAsync(const BackgroundRunner& runner)
{
    const auto moduleFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    auto load = [moduleFile]() -> std::shared_ptr<const SystemBrainDatabase> {
        const auto file = resolveEncyclopediaFile(moduleFile);
        if (!file.existsAsFile()) return nullptr;
        auto db = std::make_shared<SystemBrainDatabase>();
        if (!db->loadFromString(file.loadFileAsString().toStdString()).is_ok()) return nullptr;
        return db;
    };
    if (!runner) {
        applyLoaded(load());
        return;
    }
    // Read + parse on the processor's short pool (~130 KB of JSON), never on
    // the message thread (PLG-07: the old "loadAsync" was synchronous).
    juce::Component::SafePointer<SystemBrainMiniPanel> self(this);
    runner([self, load]() {
        auto db = load();
        juce::MessageManager::callAsync([self, db]() {
            if (self != nullptr) self->applyLoaded(db);
        });
    });
}

void SystemBrainMiniPanel::applyLoaded(std::shared_ptr<const SystemBrainDatabase> db)
{
    loading_ = false;
    db_ = std::move(db);
    available_ = db_ != nullptr;
    if (available_) {
        displayEntries_ = db_->listAll();
        rebuildRows(displayEntries_);
    }
    repaint();
    resized();
}

void SystemBrainMiniPanel::rebuildRows(const std::vector<FeatureEntry>& entries)
{
    rows_.clear();
    listContent_.removeAllChildren();
    const int w = viewport_.getWidth() > 0 ? viewport_.getWidth() : 350;
    const int contentH = static_cast<int>(entries.size()) * FeatureMiniRow::kRowHeight;
    listContent_.setSize(w, std::max(contentH, 1));

    int y = 0;
    for (const auto& e : entries) {
        auto row = std::make_unique<FeatureMiniRow>();
        row->setEntry(e);
        row->setBounds(0, y, w, FeatureMiniRow::kRowHeight);
        listContent_.addAndMakeVisible(row.get());
        rows_.push_back(std::move(row));
        y += FeatureMiniRow::kRowHeight;
    }
}

void SystemBrainMiniPanel::onSearchChanged()
{
    if (db_ == nullptr) return;
    const auto query = searchBox_.getText().toStdString();
    auto filtered = db_->search(query);
    rebuildRows(filtered);
    resized();
}

void SystemBrainMiniPanel::paint(juce::Graphics& g)
{
    g.setColour(PluginLookAndFeel::bgDark);
    g.fillRect(getLocalBounds());

    const int headerH = 22;
    auto header = getLocalBounds().removeFromTop(headerH);
    g.setColour(PluginLookAndFeel::bgPanel);
    g.fillRect(header);
    g.setColour(PluginLookAndFeel::accentTeal);
    g.setFont(khensu::ui::Palette::uiFont(10.0f, juce::Font::bold));
    g.drawText("SYSTEM BRAIN", header.reduced(8, 0), juce::Justification::centredLeft);

    if (loading_) {
        g.setColour(PluginLookAndFeel::textMuted);
        g.setFont(khensu::ui::Palette::uiFont(11.0f, juce::Font::plain));
        g.drawText("Loading...", getLocalBounds().withTrimmedTop(headerH), juce::Justification::centred);
    } else if (!available_) {
        g.setColour(PluginLookAndFeel::textMuted);
        g.setFont(khensu::ui::Palette::uiFont(11.0f, juce::Font::plain));
        g.drawText("Feature encyclopedia not found", getLocalBounds().withTrimmedTop(headerH + 28),
                   juce::Justification::centred);
    }
}

void SystemBrainMiniPanel::resized()
{
    auto b = getLocalBounds();
    b.removeFromTop(22); // header

    searchBox_.setBounds(b.removeFromTop(28).reduced(4, 2));
    b.removeFromTop(2);
    viewport_.setBounds(b);

    if (!rows_.empty()) {
        const int w = viewport_.getWidth();
        listContent_.setSize(w, static_cast<int>(rows_.size()) * FeatureMiniRow::kRowHeight);
        for (int i = 0; i < (int)rows_.size(); ++i)
            rows_[i]->setBounds(0, i * FeatureMiniRow::kRowHeight, w, FeatureMiniRow::kRowHeight);
    }
}

} // namespace khensu
