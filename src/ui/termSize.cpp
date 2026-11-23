// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <atomic>
#include <cstdint>

// C / System Headers
#include <sys/ioctl.h>
#include <unistd.h>

// Projetc Headers
#include "isocmd/ui/termSize.h"

TermSize terminalSize() {
    static std::atomic<std::uint64_t> lastGood{0};

    struct winsize ws{};

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col > 0 && ws.ws_row > 0) {
        lastGood.store(
            (std::uint64_t{ws.ws_col} << 32) | std::uint64_t{ws.ws_row},
            std::memory_order_relaxed);

        return {ws.ws_col, ws.ws_row};
    }

    const std::uint64_t v = lastGood.load(std::memory_order_relaxed);

    return {
        static_cast<std::size_t>(v >> 32),
        static_cast<std::size_t>(v & 0xFFFFFFFFu)
    };
}
