// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef UISTATE_H
#define UISTATE_H

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <memory>

// Defined in isocmd/core/sharedRefreshState.h; include that where members are used.
struct RefreshState;

/**
 * @brief Identifies which UI surface, if any, has a pending asynchronous refresh.
 *
 * Background threads cannot safely call Readline functions or write to the
 * terminal directly (Readline is not thread-safe). Instead, they publish a
 * @c PendingRefreshKind via @c GlobalState::g_pendingRefreshKind to signal
 * that a redraw is needed. The main thread's Readline event hook
 * (@c checkPendingRefresh, installed via @c rl_event_hook) polls this value
 * and performs the actual redraw itself, ensuring all Readline API calls
 * originate from the single thread that owns Readline's internal state.
 */
enum class PendingRefreshKind {
    None,     ///< No refresh pending; the hook has nothing to do.
    MainMenu, ///< Re-render the ASCII banner and main menu (e.g. after an
              ///< auto-cleared status message).
    IsoList   ///< Re-render the ISO list view via @c loadAndDisplayIso,
              ///< using the state referenced by @c GlobalState::g_pendingRefreshState.
};

namespace GlobalState {

    // State Management
    inline std::atomic<bool> isoListDirty{true};
    inline std::atomic<bool> g_pendingMenuRefresh{false};
    inline std::atomic<PendingRefreshKind> g_pendingRefreshKind{PendingRefreshKind::None};
    inline std::shared_ptr<RefreshState> g_pendingRefreshState{nullptr};
    inline std::atomic<bool> g_suppressPendingRefresh{false};
    inline std::atomic<bool> g_filteringIndicator{false};
    inline std::atomic<bool> isAtMain{true};
    inline std::atomic<bool> isAtISOList{false};
    inline std::atomic<bool> isNonInteractive{false};
    inline bool needSortingAfterflno      = false;
    inline std::size_t ITEMS_PER_PAGE          = 25;
    inline int lockFileDescriptor         = -1;

} // namespace GlobalState

#endif // UISTATE_H
