// desktop-app/Source/ui/shared/SessionScoped.h
// One session-reset fan-out (audit 2026-11-06 §10.3 / S4).
//
// Anything that holds state belonging to "the session currently open" - a
// panel's last scan rows, a loaded reference track, a blind-trial overlay -
// implements SessionScoped and registers with AppController
// (IAppController::addSessionScoped). AppController::resetSessionScopedState()
// calls resetForSession() on every registered object, on the message thread,
// every time the session changes (start / load / delete / relink). That
// replaces the per-panel ad-hoc resets that kept missing cases (DSK-02, -03,
// -04, -21): a new component only has to register to be covered.
//
// Use the RAII ScopedSessionRegistration as the LAST member of the owner so
// it deregisters first on destruction:
//
//   class MyPanel : public PanelCard, private SessionScoped {
//       void resetForSession() override { rows_.clear(); statusLabel_.setText({}, ...); }
//       ...
//       ScopedSessionRegistration sessionRegistration_{controller_, *this}; // last
//   };
//
// resetForSession() runs BEFORE the new session's analysis starts, so it must
// only drop old state (and cancel/ignore in-flight work via the session epoch),
// never start new work. THREAD_CONTEXT(UIThread)
#pragma once
#include <algorithm>
#include <vector>

namespace khensu {

class SessionScoped {
  public:
    virtual ~SessionScoped() = default;
    /// Forget everything that belongs to the previous session.
    virtual void resetForSession() = 0;
};

/// The registry AppController owns. Plain and header-only so it is
/// unit-tested without an AppController (tests/ui/SessionScopedTest.cpp).
class SessionResetRegistry final {
  public:
    void add(SessionScoped* s)
    {
        if (s != nullptr && std::find(items_.begin(), items_.end(), s) == items_.end()) items_.push_back(s);
    }
    void remove(SessionScoped* s)
    {
        items_.erase(std::remove(items_.begin(), items_.end(), s), items_.end());
    }
    /// Calls resetForSession() on every registered object. Safe against an
    /// object (de)registering others from inside its reset: it iterates a
    /// snapshot and skips anything removed meanwhile.
    void resetAll()
    {
        const auto snapshot = items_;
        for (auto* s : snapshot)
            if (std::find(items_.begin(), items_.end(), s) != items_.end()) s->resetForSession();
    }
    size_t size() const noexcept { return items_.size(); }

  private:
    std::vector<SessionScoped*> items_;
};

/// RAII registration. `Registrar` is anything with addSessionScoped /
/// removeSessionScoped (AppController, IAppController, test doubles).
template <typename Registrar> class ScopedSessionRegistrationT final {
  public:
    ScopedSessionRegistrationT(Registrar& registrar, SessionScoped& scoped) : registrar_(registrar), scoped_(scoped)
    {
        registrar_.addSessionScoped(&scoped_);
    }
    ~ScopedSessionRegistrationT() { registrar_.removeSessionScoped(&scoped_); }
    ScopedSessionRegistrationT(const ScopedSessionRegistrationT&) = delete;
    ScopedSessionRegistrationT& operator=(const ScopedSessionRegistrationT&) = delete;

  private:
    Registrar& registrar_;
    SessionScoped& scoped_;
};

} // namespace khensu
