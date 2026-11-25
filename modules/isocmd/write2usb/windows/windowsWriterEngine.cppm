// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <stddef.h>
#include <stdint.h>

export module isocmd.write2usb.windows.windowsWriterEngine;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- write2usb/windows/windowsWriterEngine ----

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
     * @param checksums Optional output of CRC-64 values, one per file in entry order.
     * @return false on the first copy failure or cancellation; true otherwise.
     */
    bool processEntries(const std::vector<write2usb_detail::IsoCopyEntry>& entries,
                        write2usb_detail::IsoType isoType,
                        const std::string& ntfsMnt,
                        size_t bufferSize,
                        char* buf,
                        std::atomic<uint64_t>& totalAccumulator,
                        std::vector<uint64_t>* checksums = nullptr);

} // namespace windowsWriter_engine::copy

} // extern "C++"
