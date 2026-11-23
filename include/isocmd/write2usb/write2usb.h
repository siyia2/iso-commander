// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef WRITE2USB_H
#define WRITE2USB_H

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

// C / System Headers
#include <sys/types.h>

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

// ------------------- ISO detection (isoDetect.cpp) -------------------

namespace iso_detect {

    /**
     * @brief Checks that a file descriptor refers to an ISO 9660 or UDF disc image.
     */
    bool isValidIso9660(int fd);

    /**
     * @brief Probes an ISO image to detect Windows 10/11 installation media.
     */
    bool isWindowsIsoInitialCheck(int fd, off_t fileSize);

} // namespace iso_detect

// ------------------- Windows ISO writer (windowsWriter.cpp) -------------------

/**
 * @brief Detects whether an ISO image is Windows installation media.
 */
bool isWindowsIso(const std::string& isoPath);

/**
 * @brief Writes a Windows-family ISO to a block device by partitioning,
 *        formatting and copying files.
 */
bool writeWindowsIsoToDevice(const std::string& isoPath,
                             const std::string& device,
                             size_t             progressIndex,
                             std::vector<ProgressInfo>& progressData);

/**
 * @struct AlignedBuffer
 * @brief RAII wrapper for an aligned I/O buffer (std::aligned_alloc).
 */
struct AlignedBuffer {
    void*  ptr  = nullptr;
    char*  data = nullptr;
    size_t size;

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

// ------------------- Internal Windows writer implementation -------------------

namespace write2usb_detail {

    /**
     * @brief Layout variants that drive partitioning and copy strategy.
     */
    enum class IsoType {
        WindowsInstall,
        FatOnly
    };

    /**
     * @brief A single file or directory discovered during the ISO tree walk.
     */
    struct IsoCopyEntry {
        std::filesystem::path src;
        std::filesystem::path dst;
        uint64_t size;
        bool isDir;
    };

} // namespace write2usb_detail

namespace windowsWriter_engine::copy {

    inline constexpr size_t kBufferSize      = 4 * 1024 * 1024;
    inline constexpr size_t kBufferAlign     = 4096;
    inline constexpr int    kPaceEveryChunks = 1;

    /**
     * @brief Iterates one entry list, creating directories and copying files.
     *
     * NTFS-bound files are pre-allocated; FAT32-bound files are not.
     *
     * @return false on the first copy failure or cancellation; true otherwise.
     */
    bool processEntries(const std::vector<write2usb_detail::IsoCopyEntry>& entries,
                        write2usb_detail::IsoType isoType,
                        const std::string& ntfsMnt,
                        size_t bufferSize,
                        char* buf,
                        std::atomic<uint64_t>& totalAccumulator);

} // namespace windowsWriter_engine::copy

#endif // WRITE2USB_H
