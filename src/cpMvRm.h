// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CPMVRM_H
#define CPMVRM_H

// C++ Standard Library Headers
#include <functional>
#include <filesystem>

// Third-Party Library Headers
#include <readline/readline.h>

// Project Headers
#include "./readline.h"
#include "./display.h"

/**
 * @struct RlFormFeedGuard
 * @brief RAII guard to restore the '\f' key binding to @c prevent_readline_keybindings.
 * * Use in functions that bind '\f' but do not modify '\t' to ensure scope integrity.
 */
struct RlFormFeedGuard {
    ~RlFormFeedGuard() { rl_bind_key('\f', prevent_readline_keybindings); }
};

/**
 * @struct RlKeyBindGuard
 * @brief RAII guard to restore both '\f' and '\t' key bindings.
 * * Typically used around paginated display logic to ensure bindings are reverted
 * even on early returns or exceptions.
 */
struct RlKeyBindGuard {
    ~RlKeyBindGuard() {
        rl_bind_key('\f', prevent_readline_keybindings);
        rl_bind_key('\t', prevent_readline_keybindings);
    }
};

/**
 * @struct RlCompleteModeGuard
 * @brief RAII guard to preserve the state of @c g_rl_complete_mode.
 */
struct RlCompleteModeGuard {
    int saved;
    RlCompleteModeGuard() : saved(RetainAndRestoreReadlineBuffer::g_rl_complete_mode) {}
    ~RlCompleteModeGuard() { RetainAndRestoreReadlineBuffer::g_rl_complete_mode = saved; }
};

/**
 * @struct RlStartupHookGuard
 * @brief RAII guard to clear @c rl_startup_hook on scope exit.
 * * Prevents stale hooks from persisting into subsequent Readline calls.
 */
struct RlStartupHookGuard {
    ~RlStartupHookGuard() { rl_startup_hook = nullptr; }
};

// ======================================================================
// Helper classes & functions
// ======================================================================

/**
 * @brief Thread-local batched reporter for success/error messages.
 *
 * Each worker thread creates its own instance. Messages are first buffered
 * locally; when the buffer reaches a threshold they are flushed under the
 * global mutex to the shared verboseSets containers. This reduces mutex
 * contention compared to locking on every message.
 *
 * @note Must call flush() when the thread finishes to ensure no messages
 *       are left in the local buffers.
 */
class BatchReporter {
public:
    /**
     * @brief Constructs a batched reporter tied to global sets.
     * @param globalIsos  Reference to the global completed operations set.
     * @param globalErrors Reference to the global failed operations set.
     * @param mutex       Mutex protecting both global sets.
     * @param batchSize   Number of messages to accumulate before auto-flushing.
     */
    BatchReporter(std::unordered_set<std::string>& globalIsos,
                  std::unordered_set<std::string>& globalErrors,
                  std::mutex& mutex,
                  size_t batchSize = 50)
        : globalIsos_(globalIsos), globalErrors_(globalErrors),
          mutex_(mutex), batchSize_(batchSize) {}

    /**
     * @brief Buffers a success message and flushes if the batch size is reached.
     * @param msg The formatted success message to add.
     */
    void addSuccess(std::string msg) {
        localIsos_.push_back(std::move(msg));
        tryFlush();
    }

    /**
     * @brief Buffers an error message and flushes if the batch size is reached.
     * @param msg The formatted error message to add.
     */
    void addError(std::string msg) {
        localErrors_.push_back(std::move(msg));
        tryFlush();
    }

    /**
     * @brief Immediately flushes all buffered messages to the global sets.
     * @details Must be called when the worker thread finishes processing
     *          to ensure no messages remain in local buffers.
     */
    void flush() {
        if (!localIsos_.empty() || !localErrors_.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            globalIsos_.insert(std::make_move_iterator(localIsos_.begin()),
                              std::make_move_iterator(localIsos_.end()));
            globalErrors_.insert(std::make_move_iterator(localErrors_.begin()),
                                std::make_move_iterator(localErrors_.end()));
            localIsos_.clear();
            localErrors_.clear();
        }
    }

private:
    /**
     * @brief Checks if either local buffer has reached the batch threshold
     *        and flushes if so.
     */
    void tryFlush() {
        if (localIsos_.size() >= batchSize_ || localErrors_.size() >= batchSize_)
            flush();
    }

    std::vector<std::string> localIsos_;          ///< Thread-local success buffer
    std::vector<std::string> localErrors_;        ///< Thread-local error buffer
    std::unordered_set<std::string>& globalIsos_; ///< Global completed set
    std::unordered_set<std::string>& globalErrors_; ///< Global failed set
    std::mutex& mutex_;                           ///< Protects global sets
    const size_t batchSize_;                      ///< Flush threshold
};

/**
 * @brief Bundles all shared state that flows through every file operation.
 *
 * Groups counters, batch reporting, the operation success flag, and an optional
 * ownership-change callback into a single object. This eliminates the need to
 * pass 8–12 separate parameters to every operation function.
 *
 * Thread‑local; one instance per worker thread.
 */
struct OperationContext {
    BatchReporter& reporter;                            ///< Batched message output
    std::atomic<size_t>* completedTasks = nullptr;      ///< Successful operation counter
    std::atomic<size_t>* failedTasks    = nullptr;      ///< Failed operation counter
    std::atomic<bool>&   operationSuccessful;           ///< Set to false on any failure
    /// Optional callback to chown newly created files to the real user.
    std::function<void(const std::filesystem::path&)> changeOwnership;
};

/**
 * @brief Builds the display source path respecting the user's display preference.
 *
 * When displayConfig::toggleNamesOnly is false, returns "srcDir/srcFile";
 * when true, returns only "srcFile". This avoids duplicating the conditional
 * logic throughout the codebase.
 *
 * @param srcDir  Source directory path.
 * @param srcFile Source filename.
 * @return The formatted display string.
 */
inline std::string buildDisplaySrc(std::string_view srcDir, std::string_view srcFile) {
    if (!displayConfig::toggleNamesOnly) {
        std::string s;
        s.reserve(srcDir.size() + srcFile.size() + 1);
        s.append(srcDir).append("/").append(srcFile);
        return s;
    }
    return std::string(srcFile);
}

#endif // CPMVRM_H
