// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef WRITE2USB_H
#define WRITE2USB_H

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>

// C / System Headers
#include <unistd.h>

void safeUmount(const char* path);

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
struct ScopedMount {
    std::string path;
    bool active = false;
    // Accepts an empty path to represent a "disabled" scope (no-op destructor).
    explicit ScopedMount(std::string p) : path(std::move(p)) {}
    // Non-copyable, non-movable — owns a unique kernel resource.
    ScopedMount(const ScopedMount&)            = delete;
    ScopedMount& operator=(const ScopedMount&) = delete;
    ScopedMount(ScopedMount&&)                 = delete;
    ScopedMount& operator=(ScopedMount&&)      = delete;
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
 * Allocates a buffer of at least `size` bytes, aligned to `alignment` bytes.
 * If `size` is not a multiple of `alignment`, it is rounded up to the next
 * multiple before allocation to satisfy std::aligned_alloc requirements.
 * The allocation is freed automatically on destruction.
 *
 * Non-copyable and non-movable — owns a unique heap allocation. Copying would
 * cause a double-free; move semantics are omitted for simplicity.
 */
struct AlignedBuffer {
    void*  ptr  = nullptr;
    char*  data = nullptr;
    size_t size; // Original requested size (actual allocation may be larger)
    AlignedBuffer(size_t size, size_t alignment) : size(size) {
        size_t remainder    = size % alignment;
        size_t adjustedSize = (remainder == 0) ? size : (size + alignment - remainder);
        ptr = std::aligned_alloc(alignment, adjustedSize);
        if (ptr) {
            data = static_cast<char*>(ptr);
        }
    }
    // Non-copyable, non-movable — owns a unique heap allocation.
    AlignedBuffer(const AlignedBuffer&)            = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    AlignedBuffer(AlignedBuffer&&)                 = delete;
    AlignedBuffer& operator=(AlignedBuffer&&)      = delete;
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
        stop();
    }
    void stop() {
        active.store(false);
        if (thread.joinable()) thread.join();
    }
};

#endif // WRITE2USB_H
