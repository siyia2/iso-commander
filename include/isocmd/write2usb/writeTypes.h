// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef WRITETYPES_H
#define WRITETYPES_H

// C++ Standard Library Headers
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

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
    std::string sourcePath;

    std::atomic<bool> completed{false};
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> bytesWritten{0};
    std::atomic<int> progress{0};
    std::atomic<double> speed{0.0};

    // Constructor
    ProgressInfo(std::string filename, std::string device, std::string totalSize,
                 std::string sourcePath)
        : filename(std::move(filename)),
          device(std::move(device)),
          totalSize(std::move(totalSize)),
          sourcePath(std::move(sourcePath)) {}

    // Move Constructor
    ProgressInfo(ProgressInfo&& other) noexcept
        : filename(std::move(other.filename)),
          device(std::move(other.device)),
          totalSize(std::move(other.totalSize)),
          sourcePath(std::move(other.sourcePath)),
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
            sourcePath = std::move(other.sourcePath);
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

/**
 * @brief Completion-tracking state shared between pooled write tasks and the UI.
 *
 * Holds the counters, mutex and condition variable that pooled write tasks
 * update and that the display thread and the main thread wait on. It is owned
 * through a @c std::shared_ptr that every task captures by value, so it stays
 * alive for tasks that outlive @ref performWriteOperation after cancellation
 * (a task must never reference these members from the function's stack).
 *
 * Neither copyable nor movable, because it contains atomics, a mutex and a
 * condition variable.
 */
struct WriteSync {
    std::atomic<size_t>     completedTasks{0};
    std::atomic<size_t>     finishedTasks{0};   // success OR failure
    std::mutex              mutex;
    std::condition_variable cv;
};

#endif // WRITETYPES_H
