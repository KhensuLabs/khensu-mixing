// desktop-app/Source/ui/shared/ReducedMotion.cpp
#include "ReducedMotion.h"
#include "../../settings/OnlineSettings.h"
#include <khensu/common/AppPaths.h>
#include <atomic>
#include <mutex>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace khensu::ui::motion {
namespace {

// -2: not read yet; -1: follow Windows; 0: off; 1: on.
std::atomic<int> g_choice{-2};
std::atomic<int> g_override{-1};
std::atomic<int> g_system{-1}; // cached Windows answer (-1 unread)
std::mutex g_mutex;

juce::File& configDirSlot()
{
    static juce::File dir;
    return dir;
}

juce::File configDir()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (configDirSlot() == juce::File()) return juce::File(juce::String(AppPaths::dataDirectory()));
    return configDirSlot();
}

int choiceFrom(const juce::String& stored)
{
    if (stored == "on") return 1;
    if (stored == "off") return 0;
    return -1;
}

int loadChoice()
{
    int c = g_choice.load(std::memory_order_acquire);
    if (c != -2) return c;
    c = choiceFrom(OnlineSettings::load(configDir()).reducedMotion);
    g_choice.store(c, std::memory_order_release);
    return c;
}

void storeChoice(int c)
{
    g_choice.store(c, std::memory_order_release);
    const juce::String value = c == 1 ? "on" : (c == 0 ? "off" : "");
    OnlineSettings::update(configDir(), [&value](OnlineSettings& s) { s.reducedMotion = value; });
}

} // namespace

bool systemPrefersReduced()
{
    int cached = g_system.load(std::memory_order_acquire);
    if (cached >= 0) return cached == 1;
#if JUCE_WINDOWS
    BOOL animations = TRUE;
    const bool read = SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) != 0;
    cached = (read && animations == FALSE) ? 1 : 0;
#else
    cached = 0;
#endif
    g_system.store(cached, std::memory_order_release);
    return cached == 1;
}

bool reduced()
{
    const int forced = g_override.load(std::memory_order_acquire);
    if (forced >= 0) return forced == 1;
    const int c = loadChoice();
    if (c >= 0) return c == 1;
    return systemPrefersReduced();
}

void setReduced(bool reducedMotion) { storeChoice(reducedMotion ? 1 : 0); }

void followSystem()
{
    g_system.store(-1, std::memory_order_release); // re-read Windows on the next ask
    storeChoice(-1);
}

bool followsSystem() { return loadChoice() < 0; }

void setConfigDirectory(const juce::File& dir)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        configDirSlot() = dir;
    }
    g_choice.store(-2, std::memory_order_release);
}

void setOverrideForTesting(int forced) { g_override.store(forced < 0 ? -1 : (forced > 0 ? 1 : 0)); }

} // namespace khensu::ui::motion
