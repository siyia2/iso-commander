// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Declares synchronization markers and editor-session hooks for PTY tests.
 */

#pragma once

namespace isocmd_tests::terminal_sync {

	// Non-printing, test-only PTY markers. Never emitted by the release binary.
	inline constexpr char input[] = "\033]777;ISOCMD-TEST-INPUT\007";
	inline constexpr char pause[] = "\033]777;ISOCMD-TEST-PAUSE\007";
	unsigned long lineEditorSession() noexcept;
	void lineEditorStarted() noexcept;
}
