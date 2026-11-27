// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Line editor guards for USB mapping and confirmation prompts.
 */

export module isocmd.write2usb.util.terminalGuards;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

/**
 * @brief Configures the write-mapping prompt and restores standard editor bindings.
 *
 * Establishes a known editor configuration rather than saving arbitrary
 * previous settings. Signal disposition and terminal EOF settings are
 * managed separately from editor keybindings.
 */
struct TerminalStateGuard {
	/**
	 * @brief Installs mapping-prompt bindings, ignores SIGINT and attempts to disable terminal VEOF.
	 * Also requests a screen and scrollback clear.
	 */
	TerminalStateGuard();

	/**
	 * @brief Restores standard editor bindings and completion hooks.
	 * @note Does not restore the previous SIGINT disposition or terminal VEOF.
	 *       restoreLineEditor() reinstates a known configuration, not a snapshot.
	 */
	~TerminalStateGuard();

	TerminalStateGuard(const TerminalStateGuard&) = delete;
	TerminalStateGuard& operator=(const TerminalStateGuard&) = delete;
};

/**
 * @brief Temporarily disables completion, clearing and history navigation.
 *
 * Reapplies the mapping-prompt editor configuration on scope exit, including
 * early returns and exception unwinding. Does not snapshot prior bindings.
 */
struct ConfirmationModeGuard {
	/// Switches line editor into confirmation-prompt mode.
	ConfirmationModeGuard();

	/**
	 * @brief Reapplies mapping-prompt bindings and completion hooks.
	 * Allows the mapping prompt to resume after a rejected confirmation.
	 */
	~ConfirmationModeGuard();

	ConfirmationModeGuard(const ConfirmationModeGuard&) = delete;
	ConfirmationModeGuard& operator=(const ConfirmationModeGuard&) = delete;

private:
	/// Reinstalls the shared mapping-prompt editor configuration.
	static void setupLineEditor();
};

} // extern "C++"
