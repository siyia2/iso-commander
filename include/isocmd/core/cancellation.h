// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CANCELLATION_H
#define CANCELLATION_H

// C++ Standard Library Headers
#include <atomic>

namespace GlobalState {

    /// Set (e.g. by the SIGINT handler) to ask long-running operations to stop.
    inline std::atomic<bool> g_operationCancelled{false};

} // namespace GlobalState

#endif // CANCELLATION_H
