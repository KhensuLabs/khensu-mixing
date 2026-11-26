// plugin-copilot/Source/monitor/MonitorSolo.h
// ENH-10 (2): the monitoring solo. STU-5 moved it to shared-core
// (khensu/dsp/MonitorSolo.h) so the desktop transport's Mono / Side / Sub /
// Dim run the same crossovers; this name stays for the plugin's code.
#pragma once

#include <khensu/dsp/MonitorSolo.h>

namespace khensu::copilot {
using MonitorSolo = khensu::dsp::MonitorSolo;
} // namespace khensu::copilot
