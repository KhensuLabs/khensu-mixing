// desktop-app/Source/ui/Settings/DisplayPrefs.h
// BUG-6: the chosen theme, kept in display.json in the app data folder beside
// settings.json (which the service reads and has no use for it). Reduced
// motion keeps its own setting (ui/shared/ReducedMotion.h). Header-only so the
// tests read and write it without the screen. THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h"
#include <optional>

namespace khensu::display_prefs {

struct Prefs {
    std::optional<KhensuTheme::Theme> theme; ///< empty: never chosen (keep the default)
};

inline juce::File fileIn(const juce::File& configDir) { return configDir.getChildFile("display.json"); }

/// The stored id of a theme ("dark" / "high_contrast" / "light") - data, not text shown.
inline const char* themeId(KhensuTheme::Theme t) noexcept
{
    switch (t) {
    case KhensuTheme::Theme::Dark: return "dark";
    case KhensuTheme::Theme::HighContrast: return "high_contrast";
    case KhensuTheme::Theme::Light: return "light";
    }
    return "dark";
}

inline std::optional<KhensuTheme::Theme> themeFromId(const juce::String& id)
{
    for (auto t : {KhensuTheme::Theme::Dark, KhensuTheme::Theme::HighContrast, KhensuTheme::Theme::Light})
        if (id == themeId(t)) return t;
    return std::nullopt;
}

/// Missing or unreadable file: no theme chosen.
inline Prefs load(const juce::File& configDir)
{
    Prefs p;
    const auto file = fileIn(configDir);
    if (!file.existsAsFile()) return p;
    const auto v = juce::JSON::parse(file.loadFileAsString());
    if (!v.isObject()) return p;
    p.theme = themeFromId(v["theme"].toString());
    return p;
}

inline bool save(const juce::File& configDir, const Prefs& p)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    if (p.theme.has_value()) o->setProperty("theme", juce::String(themeId(*p.theme)));
    if (!configDir.isDirectory() && !configDir.createDirectory()) return false;
    return fileIn(configDir).replaceWithText(juce::JSON::toString(juce::var(o.get())));
}

} // namespace khensu::display_prefs
