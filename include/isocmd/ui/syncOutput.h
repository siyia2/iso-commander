// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SYNCOUTPUT_H
#define SYNCOUTPUT_H

// C++ Standard Library Headers
#include <string_view>

/// Synchronized-output (DEC mode 2026) control sequences, kept as named
/// constants so callers that write the sequences by hand stay in sync
/// with @ref SyncOutput.
inline constexpr bool             kUseSynchronizedOutput = true;
inline constexpr std::string_view kSyncBegin             = "\033[?2026h";
inline constexpr std::string_view kSyncEnd               = "\033[?2026l";

/**
 * @brief RAII guard that brackets a terminal repaint in DEC mode 2026
 *        (synchronized output).
 *
 * Writes @ref kSyncBegin (`CSI ? 2026 h`) on construction and @ref kSyncEnd
 * (`CSI ? 2026 l`) on destruction, so every `std::cout` write between the two
 * is held by the terminal emulator and presented as a single atomic frame.
 * Terminals that do not implement mode 2026 typically ignore the sequences.
 *
 * The destructor flushes, because the commit only happens when the closing
 * sequence actually reaches the terminal; a buffered `?2026l` is not a commit.
 *
 * Non-copyable and non-movable: one guard owns one envelope. Do not nest two
 * `SyncOutput` objects around code that mixes `std::cout` and `std::cerr`.
 */
struct SyncOutput {
    SyncOutput();
    ~SyncOutput();

    SyncOutput(const SyncOutput&)            = delete;
    SyncOutput& operator=(const SyncOutput&) = delete;
};

#endif // SYNCOUTPUT_H
