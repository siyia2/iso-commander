// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef WRITE_H
#define WRITE_H

// C++ Standard Library Headers
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility> // For std::move

// System Headers
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>

namespace fs = std::filesystem;

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
