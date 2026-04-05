// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef WRITE_H
#define WRITE_H


/**
 * @brief Information about an ISO image file.
 */
struct IsoInfo {
    /** @brief Full filesystem path to the ISO file. */
    std::string path;
    
    /** @brief Name of the file without path. */
    std::string filename;
    
    /** @brief Size of the ISO in bytes. */
    uint64_t size;
    
    /** @brief Human-readable size string (e.g., "4.2 GB"). */
    std::string sizeStr;
    
    /** @brief Original sort index for maintaining list order. */
    size_t originalIndex;
};

/**
 * @brief Canonical list of all supported configuration settings with validation.
 * @details Manages the state of an ongoing or completed write process, 
 * utilizing atomics for thread-safe progress updates.
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

    ProgressInfo(std::string filename, std::string device, std::string totalSize)
        : filename(std::move(filename)),
          device(std::move(device)),
          totalSize(std::move(totalSize)) {}

    ProgressInfo(ProgressInfo&& other) noexcept
        : filename(std::move(other.filename)),
          device(std::move(other.device)),
          totalSize(std::move(other.totalSize)),
          completed(other.completed.load()),
          failed(other.failed.load()),
          bytesWritten(other.bytesWritten.load()),
          progress(other.progress.load()),
          speed(other.speed.load()) {}

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

    ProgressInfo(const ProgressInfo&) = delete;
    ProgressInfo& operator=(const ProgressInfo&) = delete;
};

#endif // WRITE_H
