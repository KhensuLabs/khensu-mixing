// plugin-copilot/Source/ui/SystemBrainMiniPanel.h
#pragma once
#include <JuceHeader.h>
#include <khensu/systembrain/SystemBrainDatabase.h>
#include <khensu/systembrain/FeatureEntry.h>
#include <functional>
#include <memory>
#include <vector>

namespace khensu {

class FeatureMiniRow final : public juce::Component {
  public:
    FeatureMiniRow() = default;
    void setEntry(const FeatureEntry& e);
    void paint(juce::Graphics& g) override;
    void resized() override {}
    static constexpr int kRowHeight = 48;

  private:
    juce::String name_, category_, shortDesc_;
    bool pluginAvailable_ = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FeatureMiniRow)
};

class SystemBrainMiniPanel final : public juce::Component {
  public:
    /// Runs a job off the message thread (the processor's short pool). Empty
    /// = load synchronously (tests without a processor).
    using BackgroundRunner = std::function<void(std::function<void()>)>;

    explicit SystemBrainMiniPanel(BackgroundRunner runner = {});
    ~SystemBrainMiniPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    static constexpr const char* kEncyclopediaFileName = "feature_encyclopedia.json";

    /// Where the feature encyclopedia lives for a plugin module at
    /// `moduleFile` (PLG-07), first match wins:
    ///   1. inside the plugin bundle: <bundle>/Contents/Resources (shipped by
    ///      CMake; `moduleFile` is <bundle>/Contents/<arch>/<binary>);
    ///   2. a source/build tree: resources/system_brain/ beside any ancestor
    ///      of the module (developer builds);
    ///   3. the desktop install's data directory (AppPaths::dataDirectory()).
    /// The module path comes from File::currentExecutableFile - the DLL, not
    /// the host EXE that currentApplicationFile used to return. Returns a
    /// non-existent File when nothing is found.
    static juce::File resolveEncyclopediaFile(const juce::File& moduleFile);

    bool isAvailableForTest() const noexcept { return available_; }
    bool isLoadingForTest() const noexcept { return loading_; }
    size_t numEntriesForTest() const noexcept { return displayEntries_.size(); }

  private:
    std::shared_ptr<const SystemBrainDatabase> db_;
    std::vector<FeatureEntry> displayEntries_;
    juce::TextEditor searchBox_;
    juce::Viewport viewport_;
    juce::Component listContent_;
    std::vector<std::unique_ptr<FeatureMiniRow>> rows_;
    bool loading_ = true;
    bool available_ = false;

    void loadAsync(const BackgroundRunner& runner);
    void applyLoaded(std::shared_ptr<const SystemBrainDatabase> db);
    void rebuildRows(const std::vector<FeatureEntry>& entries);
    void onSearchChanged();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SystemBrainMiniPanel)
};

} // namespace khensu
