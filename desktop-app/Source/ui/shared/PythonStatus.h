// desktop-app/Source/ui/shared/PythonStatus.h
#pragma once

namespace khensu {

/// The local Python service as the UI shows it (sidebar dot, Dashboard
/// health widget). Own header so the service interfaces can name it
/// without pulling in SidebarComponent.
enum class PythonStatus { Stopped, Starting, Running, GaveUp };

} // namespace khensu
