// desktop-app/Source/ui/shared/ListenerHandle.h
#pragma once
#include <functional>
#include <utility>

namespace khensu {

/// RAII registration with a keyed listener hub (ENG-2): destroying the handle
/// deregisters. A component keeps one per registration as a member declared
/// AFTER everything its callback touches, so the registration ends before
/// any of that is destroyed - and no destructor can forget the matching
/// remove*Listener call, which left a hub calling into a dead panel.
/// Move-only.
class ListenerHandle {
  public:
    ListenerHandle() = default;
    explicit ListenerHandle(std::function<void()> remove) : remove_(std::move(remove)) {}
    ListenerHandle(ListenerHandle&& other) noexcept : remove_(std::exchange(other.remove_, nullptr)) {}
    ListenerHandle& operator=(ListenerHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            remove_ = std::exchange(other.remove_, nullptr);
        }
        return *this;
    }
    ListenerHandle(const ListenerHandle&) = delete;
    ListenerHandle& operator=(const ListenerHandle&) = delete;
    ~ListenerHandle() { reset(); }

    /// Deregisters now (idempotent).
    void reset()
    {
        if (auto remove = std::exchange(remove_, nullptr)) remove();
    }
    bool isRegistered() const noexcept { return remove_ != nullptr; }

  private:
    std::function<void()> remove_;
};

/// Registers `callback` under `key` through the hub's add/remove pair and
/// returns the handle that deregisters it:
///
///     treeListener_ = listen(controller_.renders(), this, &IRenders::addRenderTreeListener,
///                            &IRenders::removeRenderTreeListener, [this] { refresh(); });
///
/// The hub must outlive the handle - true for every service, which the shell
/// owns and destroys after the screens.
template <typename Hub, typename AddFn, typename RemoveFn, typename Callback>
[[nodiscard]] ListenerHandle listen(Hub& hub, void* key, AddFn add, RemoveFn remove, Callback&& callback)
{
    (hub.*add)(key, std::forward<Callback>(callback));
    return ListenerHandle([&hub, key, remove] { (hub.*remove)(key); });
}

} // namespace khensu
