// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.write2usb.util.terminalGuards;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- write2usb/util/terminalGuards ----
/**
 * @brief RAII guard for the readline/terminal state that needs to be set up
 *        at the start of every loop iteration and torn down on every exit
 *        path (return, continue, or exception).
 */
struct TerminalStateGuard {
    /**
     * @brief Configures readline bindings/hooks and disables SIGINT /
     *        Ctrl-D for the duration of the guard's lifetime.
     */
    TerminalStateGuard();

    /**
     * @brief Restores readline bindings/hooks via restoreReadline().
     *
     * @note restoreReadline() only resets readline bindings/hooks. The
     *       SIGINT = SIG_IGN and disable_ctrl_d() set in the constructor
     *       are NOT undone here, matching the original code's behavior
     *       (it never restored these at any exit point either). If that's
     *       actually a latent bug, it's pre-existing and separate from
     *       this refactor.
     */
    ~TerminalStateGuard();

    TerminalStateGuard(const TerminalStateGuard&) = delete;
    TerminalStateGuard& operator=(const TerminalStateGuard&) = delete;
};

/**
 * @brief RAII guard for the brief window where readline is reconfigured to
 *        ask the y/n confirmation question.
 *
 * Restores the *outer* (TerminalStateGuard) readline configuration on
 * exit, regardless of how this scope is left.
 */
struct ConfirmationModeGuard {
    /**
     * @brief Switches readline into confirmation-prompt mode.
     */
    ConfirmationModeGuard();

    /**
     * @brief Re-applies the main mappings-prompt readline configuration.
     *
     * Ensures that if the user says "no" and the outer loop continues,
     * the next TerminalStateGuard iteration (or the existing one, if we
     * don't reconstruct it) is in the expected state.
     */
    ~ConfirmationModeGuard();

    ConfirmationModeGuard(const ConfirmationModeGuard&) = delete;
    ConfirmationModeGuard& operator=(const ConfirmationModeGuard&) = delete;

private:
    /**
     * @brief Mirrors the original setupReadline() lambda so this guard
     *        doesn't depend on a free function with the same name living
     *        elsewhere.
     */
    static void setupReadline();
};

} // extern "C++"
