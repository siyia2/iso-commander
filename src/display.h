// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DISPLAY_H
#define DISPLAY_H

/**
 * @brief Canonical list of all supported configuration settings with validation.
 * @details Namespace containing global toggles that control the verbosity and
 * detail level of various list-based operations within the UI.
 */
inline namespace displayConfig {
    /** @brief Toggle to show the full list during mount operations. */
    inline bool toggleFullListMount = false;

    /** @brief Toggle to show the compact list during unmount operations. */
    inline bool toggleFullListUmount = true;

    /** @brief Toggle to show the compact list during file operations (Cp/Mv/Rm). */
    inline bool toggleFullListCpMvRm = true;

    /** @brief Toggle to show the full list during write operations. */
    inline bool toggleFullListWrite2usb = false;

    /** @brief Toggle to show the full list during image convert2isos. */
    inline bool toggleFullListConvert2iso = false;

    /** @brief Toggle to display only filenames, hiding full directory paths. */
    inline bool toggleNamesOnly = false;
}

/**
 * @brief RAII guard that brackets a terminal repaint in DEC mode 2026
 *        (synchronized output).
 *
 * Writes `CSI ? 2026 h` on construction and `CSI ? 2026 l` on destruction,
 * so every `std::cout` write between the two is held by the terminal
 * emulator and presented as a single atomic frame. Terminals that do not
 * implement 2026 ignore the sequences as no-ops (per the VT spec unknown
 * DEC private modes are dropped), and every emulator that does implement
 * it has a watchdog (~1 s) that auto-commits if the matching `l` never
 * arrives — so a throwing code path between construction and destruction
 * cannot leave the terminal permanently frozen, only briefly stalled.
 *
 * The destructor flushes because the commit only happens when the closing
 * sequence reaches the terminal — a buffered `?2026l` is not a commit.
 *
 * Non-copyable and non-movable: one guard owns one envelope. Do not nest
 * two `SyncOutput` objects around code that mixes `std::cout` and
 * `std::cerr` — `std::cerr` is unbuffered and unitbuf, so a nested
 * envelope can split the frame and reorder the two streams.
 */
struct SyncOutput {
    SyncOutput()  { std::cout << "\033[?2026h"; }
    ~SyncOutput() { std::cout << "\033[?2026l" << std::flush; }

    SyncOutput(const SyncOutput&)            = delete;
    SyncOutput& operator=(const SyncOutput&) = delete;
};

#endif // DISPLAY_H
