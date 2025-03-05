// SPDX-License-Identifier: GNU General Public License v2.0

#ifndef WRITE_H
#define WRITE_H


// IsoInfo structure
struct IsoInfo {
    std::string path;
    std::string filename;
    uint64_t size;
    std::string sizeStr;
    size_t originalIndex;
};


// Progress tracking structure
struct ProgressInfo {
    std::string filename;
    std::string device;
    std::string totalSize;

    // Atomic members for tracking progress
    std::atomic<bool> completed{false};
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> bytesWritten{0};
    std::atomic<int> progress{0};
    std::atomic<double> speed{0.0};

    // Constructor to initialize members
    ProgressInfo(std::string filename, std::string device, std::string totalSize)
        : filename(std::move(filename)),
          device(std::move(device)),
          totalSize(std::move(totalSize)) {}

    // Explicitly define the move constructor
    ProgressInfo(ProgressInfo&& other) noexcept
        : filename(std::move(other.filename)),
          device(std::move(other.device)),
          totalSize(std::move(other.totalSize)),
          completed(other.completed.load()),
          failed(other.failed.load()),
          bytesWritten(other.bytesWritten.load()),
          progress(other.progress.load()),
          speed(other.speed.load()) {}

    // Explicitly define the move assignment operator
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

    // Delete the copy constructor and copy assignment operator
    ProgressInfo(const ProgressInfo&) = delete;
    ProgressInfo& operator=(const ProgressInfo&) = delete;
};


#endif // WRITE_H
