// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef WRITE2USB_H
#define WRITE2USB_H

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

// ------------------- Forward declarations -------------------

/**
 * @brief Per-device progress slot (progress, speed, bytes written, and
 *        completed/failed flags).
 *
 * Only forward-declared here so this header stays independent of include
 * order. The full definition lives elsewhere in the project (e.g.
 * write2usbUI.h or state.h); any translation unit that actually accesses
 * its members must include that header.
 */
struct ProgressInfo;

// ------------------- Windows ISO writer (windowsWriter.cpp) -------------------

/**
 * @brief Writes a Windows-family ISO to a block device by partitioning,
 *        formatting and copying files (rather than a raw image copy).
 *
 * Wipes and repartitions the device as GPT: for full Windows install media a
 * FAT32 ESP (WINBOOT, 1 GiB) plus an NTFS data partition (WINDATA); for
 * WinPE/rescue disks a single FAT32 partition (WINPE). ISO contents are then
 * copied file by file, with O_DIRECT for FAT32 and buffered I/O for NTFS,
 * using double buffering and a background progress monitor. Symlinks in the
 * ISO are skipped. See the implementation comment in windowsWriter.cpp for
 * the full behavioural contract.
 *
 * @param isoPath       Path to the source ISO file.
 * @param device        Target block device (e.g. /dev/sdb). Will be wiped.
 *                      Partition nodes are derived as device + N, so devices
 *                      needing a "p" separator (nvme, mmcblk) are unsupported.
 * @param progressIndex Index of this job's slot in @p progressData.
 * @param progressData  Progress array used for live progress, speed and
 *                      completion reporting.
 * @return true on success; false on error, truncated source or cancellation.
 *         The slot's failed flag is set on genuine errors only, not on
 *         cancellation.
 */
bool writeWindowsIsoToDevice(const std::string& isoPath,
                             const std::string& device,
                             size_t             progressIndex,
                             std::vector<ProgressInfo>& progressData);

/**
 * @struct AlignedBuffer
 * @brief RAII wrapper for an aligned I/O buffer (std::aligned_alloc).
 *
 * Allocates a buffer of at least `size` bytes, aligned to `alignment` bytes.
 * If `size` is not a multiple of `alignment`, it is rounded up to the next
 * multiple before allocation to satisfy std::aligned_alloc requirements.
 * The allocation is freed automatically on destruction. Check ok() (or the
 * bool conversion) after construction: `data` is nullptr if allocation failed.
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
 * monitor is always torn down — even on early returns or exceptions. stop()
 * may also be called explicitly and is safe to call more than once.
 *
 * Non-copyable. Holds references, so the thread and flag must outlive the
 * guard.
 */
struct ThreadGuard {
    std::thread&       thread;
    std::atomic<bool>& active;
    ThreadGuard(std::thread& t, std::atomic<bool>& a) : thread(t), active(a) {}
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
