// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef WRITE_H
#define WRITE_H

// C++ Standard Library Headers
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unordered_set>

// C / System Headers
#include <unistd.h>

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

// ------------------- Windows ISO Writer RAII -------------------
/**
 * @struct ScopedMount
 * @brief RAII wrapper to ensure automatic unmounting and directory removal.
 *
 * Manages the lifecycle of a mount point created via mkdtemp. On destruction:
 * - If active (setMounted() was called), safeUmount is called first.
 * - rmdir is always called if path is non-empty, cleaning up the temp dir
 *   regardless of whether a mount succeeded.
 */

void safeUmount(const char* path);

struct ScopedMount {
    std::string path;
    bool active = false;

    // Accepts an empty path to represent a "disabled" scope (no-op destructor).
    explicit ScopedMount(std::string p) : path(std::move(p)) {}

    // Non-copyable, non-movable — owns a unique kernel resource.
    ScopedMount(const ScopedMount&)            = delete;
    ScopedMount& operator=(const ScopedMount&) = delete;

    // Call after a successful mount to arm the safeUmount on destruction.
    void setMounted() { active = true; }

    ~ScopedMount() {
        if (path.empty()) return;
        if (active) safeUmount(path.c_str());
        rmdir(path.c_str());
    }
};

/**
 * @struct AlignedBuffer
 * @brief RAII wrapper for a posix_memalign'd I/O buffer.
 *
 * Allocates a buffer aligned to `alignment` bytes of size `size` bytes.
 * The allocation is freed automatically on destruction.
 */
struct AlignedBuffer {
    void* ptr  = nullptr;
    char* data = nullptr;
    size_t size; // Store size to fulfill alignment requirements

    AlignedBuffer(size_t size, size_t alignment) : size(size) {
        size_t remainder = size % alignment;
        size_t adjustedSize = (remainder == 0) ? size : (size + alignment - remainder);

        ptr = std::aligned_alloc(alignment, adjustedSize);
        if (ptr) {
            data = static_cast<char*>(ptr);
        }
    }

    ~AlignedBuffer() { std::free(ptr); }

    bool ok() const { return data != nullptr; }
    explicit operator bool() const { return ok(); }
};

/**
 * @struct ThreadGuard
 * @brief RAII wrapper that stops and joins a background thread on destruction.
 *
 * Sets the `active` flag to false and joins the thread, ensuring the progress
 * monitor is always torn down — even on early returns or exceptions.
 */
struct ThreadGuard {
    std::thread&       thread;
    std::atomic<bool>& active;

    ThreadGuard(std::thread& t, std::atomic<bool>& a) : thread(t), active(a) {}

    // Non-copyable, non-movable.
    ThreadGuard(const ThreadGuard&)            = delete;
    ThreadGuard& operator=(const ThreadGuard&) = delete;

    ~ThreadGuard() {
        active.store(false);
        if (thread.joinable()) thread.join();
    }
};

// ---------- Used exclusively for the [FLUSING] device indicator ------------

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

#endif // WRITE_H
