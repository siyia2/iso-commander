// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef WRITE2USBUI_H
#define WRITE2USBUI_H

// C++ Standard Library Headers
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unordered_set>

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "./inputHandling.h"
#include "./readline.h"

// --- Data Structures ---

/**
 * @brief Information about an ISO image file.
 */
struct IsoInfo {
    std::string path;          /** Full filesystem path to the ISO file. */
    std::string filename;      /** Name of the file without path. */
    uint64_t size;             /** Size of the ISO in bytes. */
    std::string sizeStr;       /** Human-readable size string (e.g., "4.2 GB"). */
    size_t originalIndex;      /** Original sort index for maintaining list order. */
};

/**
 * @brief Thread-safe state tracker for an ongoing write process.
 * @details Utilizes atomics for progress updates and move-only semantics to
 * prevent accidental copying of atomic states.
 */
struct ProgressInfo {
    std::string filename;
    std::string device;
    std::string totalSize;

    std::atomic<bool> completed{false};
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> bytesWritten{0};
    std::atomic<int> progress{0};
    std::atomic<double> speed{0.0};

    // Constructor
    ProgressInfo(std::string filename, std::string device, std::string totalSize)
        : filename(std::move(filename)),
          device(std::move(device)),
          totalSize(std::move(totalSize)) {}

    // Move Constructor
    ProgressInfo(ProgressInfo&& other) noexcept
        : filename(std::move(other.filename)),
          device(std::move(other.device)),
          totalSize(std::move(other.totalSize)),
          completed(other.completed.load()),
          failed(other.failed.load()),
          bytesWritten(other.bytesWritten.load()),
          progress(other.progress.load()),
          speed(other.speed.load()) {}

    // Move Assignment
    ProgressInfo& operator=(ProgressInfo&& other) noexcept {
        if (this != &other) {
            filename = std::move(other.filename);
            device = std::move(other.device);
            totalSize = std::move(other.totalSize);
            completed.store(other.completed.load());
            failed.store(other.failed.load());
            bytesWritten.store(other.bytesWritten.load());
            progress.store(other.progress.load());
            speed.store(other.speed.load());
        }
        return *this;
    }

    // Disable Copying (Atomics cannot be copied)
    ProgressInfo(const ProgressInfo&) = delete;
    ProgressInfo& operator=(const ProgressInfo&) = delete;
};

inline std::vector<ProgressInfo> progressData; ///< Shared progress state for all active write tasks.

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
    TerminalStateGuard() {
        rl_completion_display_matches_hook = [](char **matches, int num_matches, int max_length) {
            (void)matches;
            (void)num_matches;
            (void)max_length;
        };

        rl_attempted_completion_function = completion_cb;
        rl_bind_key('\t', rl_complete);
        rl_bind_key('\f', clear_screen_and_buffer);
        rl_bind_keyseq("\033[A", rl_get_previous_history);
        rl_bind_keyseq("\033[B", rl_get_next_history);
        rl_bind_keyseq("\\e[5~", rl_named_function("previous-history"));
        rl_bind_keyseq("\\e[6~", rl_named_function("next-history"));

        signal(SIGINT, SIG_IGN);
        disable_ctrl_d();
        clearScrollBuffer();
    }

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
    ~TerminalStateGuard() {
        restoreReadline();
    }

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
    ConfirmationModeGuard() {
        disableReadlineForConfirmation();
    }

    /**
     * @brief Re-applies the main mappings-prompt readline configuration.
     *
     * Ensures that if the user says "no" and the outer loop continues,
     * the next TerminalStateGuard iteration (or the existing one, if we
     * don't reconstruct it) is in the expected state.
     */
    ~ConfirmationModeGuard() {
        setupReadline();
    }

    ConfirmationModeGuard(const ConfirmationModeGuard&) = delete;
    ConfirmationModeGuard& operator=(const ConfirmationModeGuard&) = delete;

private:
    /**
     * @brief Mirrors the original setupReadline() lambda so this guard
     *        doesn't depend on a free function with the same name living
     *        elsewhere.
     */
    static void setupReadline() {
        rl_completion_display_matches_hook = [](char **matches, int num_matches, int max_length) {
            (void)matches;
            (void)num_matches;
            (void)max_length;
        };

        rl_attempted_completion_function = completion_cb;
        rl_bind_key('\t', rl_complete);
        rl_bind_key('\f', clear_screen_and_buffer);
        rl_bind_keyseq("\033[A", rl_get_previous_history);
        rl_bind_keyseq("\033[B", rl_get_next_history);
        rl_bind_keyseq("\\e[5~", rl_named_function("previous-history"));
        rl_bind_keyseq("\\e[6~", rl_named_function("next-history"));
    }
};

// ---------- Used exclusively for the [FLUSHING] device indicator ------------

/**
 * @brief A thread-safe manager for tracking and joining background tasks.
 * * This structure acts as a registry for detached-style background threads,
 * allowing the application to track them and ensure they are joined gracefully
 * during the application's shutdown sequence. This prevents crashes related
 * to global object destruction while background threads are still active.
 */
struct DrainingThreadManager {
    std::vector<std::thread> threads; ///< Internal storage for registered threads.
    std::mutex mtx;                  ///< Mutex to ensure thread-safe registration and access.

    /**
     * @brief Adds a thread to the manager for lifecycle tracking.
     * * @param t An rvalue reference to the std::thread to be managed.
     * The manager takes ownership of the thread.
     */
    void add(std::thread&& t) {
        std::lock_guard<std::mutex> lock(mtx);
        threads.emplace_back(std::move(t));
    }

    /**
     * @brief Blocks until all registered threads have completed their execution.
     * * Iterates through the registered threads, verifies they are joinable,
     * and performs a join operation. The internal vector is cleared
     * upon completion to prevent duplicate joins.
     */
    void joinAll() {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads.clear();
    }
};

// Global instances
inline DrainingThreadManager g_drainingManager;
inline std::mutex g_drainingDevicesMutex;
inline std::unordered_set<std::string> g_drainingDevices;
inline std::atomic<bool> g_drainingCancelled{false};

// ---------- End of [FLUSHING] device indicator block ------------

/**
 * Performs the actual write2usb.
 */
bool writeIsoToDevice(const std::string& isoPath, const std::string& device, size_t progressIndex);

// --- Filesystem & Formatting Utilities ---

/**
 * Converts a raw byte count into a human-readable string (KB, MB, GB).
 */
std::string formatFileSize(uint64_t size);

/**
 * Formats a speed value into a readable string (e.g., "15.4 MB/s").
 */
std::string formatSpeed(double mbPerSec);

/**
 * Queries the kernel for the total capacity of a block device.
 * @param device Path to the device (e.g., "/dev/sdb").
 * @return Size in bytes.
 */
uint64_t getBlockDeviceSize(const std::string& device);

#endif // WRITE2USBUI_H
