#include "MainWindow.h"
#include "AppController.h"
#include "ui/shared/KhensuTheme.h"
#include "settings/OnlineSettings.h"
#include <khensu/common/AppPaths.h>

namespace khensu {

MainWindow::MainWindow(const juce::String& name)
    : DocumentWindow(name,
                     KhensuTheme::bgBase, // dark background
                     DocumentWindow::allButtons)
{
    setUsingNativeTitleBar(true);
    setContentOwned(new AppController(), true);
    setResizable(true, true);
    // DSK-16: below this the fixed-width header controls and the two-column
    // Dashboard grid get negative / zero bounds.
    setResizeLimits(1024, 640, 16384, 16384);
    centreWithSize(1440, 900);
    setVisible(true);
#if JUCE_WINDOWS
    // CE-PLT-4: JUCE 8 gives every Windows peer the Direct2D renderer (engine
    // index 1). Nothing in the app disables it; the one way off is the user's
    // Settings opt-out (software GDI fallback for a bad driver), applied here
    // at launch - the Settings screen alone only applied it once it had a peer.
    if (auto* peer = getPeer()) {
        const auto settings = OnlineSettings::load(juce::File(juce::String(AppPaths::dataDirectory())));
        if (!settings.useGpuRendering) peer->setCurrentRenderingEngine(0);
        const auto engines = peer->getAvailableRenderingEngines();
        const int current = peer->getCurrentRenderingEngine();
        juce::Logger::writeToLog("MainWindow: rendering engine " +
                                 (juce::isPositiveAndBelow(current, engines.size()) ? engines[current]
                                                                                    : juce::String(current)));
    }
#endif
}

void MainWindow::closeButtonPressed() { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

} // namespace khensu
