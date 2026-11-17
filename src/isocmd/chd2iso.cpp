/* SPDX-License-Identifier: BSD-3-Clause */

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// C / System Headers
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <unistd.h>

// Project Headers
#include "../chd.h"
#include "../state.h"

namespace fs = std::filesystem;

/**
 * @brief Detects the correct user data offset within a CD-ROM sector layout.
 *
 * Reads logical sector 16 (the ISO9660 Primary Volume Descriptor) and searches
 * for the "CD001" signature to determine the correct user data offset for this
 * specific image layout. Supports both legacy and modern CHD formats.
 *
 * @param chd            Pointer to the opened CHD file.
 * @param hunkSize       Size of each hunk in bytes.
 * @param rawSectorSize  Size of each raw sector (2048, 2352, or 2448 bytes).
 * @param sectorsPerHunk Number of sectors contained within a single hunk.
 *
 * @return The detected user data offset in bytes (0, 16, or 24), or a sensible fallback offset if detection fails.
 */
static uint32_t detectUserDataOffset(chd_file* chd, uint32_t hunkSize, uint32_t rawSectorSize, uint32_t sectorsPerHunk) {
    if (rawSectorSize == 2048) return 0;

    uint32_t targetHunk = 16 / sectorsPerHunk;
    uint32_t sectorIndex = 16 % sectorsPerHunk;
    std::vector<uint8_t> testBuf(hunkSize);

    if (chd_read(chd, targetHunk, testBuf.data()) == CHDERR_NONE) {
        const uint8_t* s16 = testBuf.data() + (sectorIndex * rawSectorSize);
        std::vector<uint32_t> candidates = (rawSectorSize == 2352) ? std::vector<uint32_t>{16, 24} : std::vector<uint32_t>{0, 16, 24};

        for (uint32_t off : candidates) {
            if (std::memcmp(s16 + off + 1, "CD001", 5) == 0) {
                return off;
            }
        }
    }
    return (rawSectorSize == 2352) ? 16 : 0;
}

/**
 * @brief Performs single-threaded, buffered writing for CHD to ISO conversion.
 *
 * Used primarily for smaller files (leq 1 GiB) to minimize overhead. Decompresses
 * hunks sequentially, extracts user data sectors based on the detected offset,
 * and streams them to disk using a buffered ofstream.
 *
 * @param rawChd         Pointer to the opened CHD file.
 * @param isoPath        Path where the output ISO file will be written.
 * @param hunkSize       Size of each hunk in bytes.
 * @param rawSectorSize  Size of each raw sector in bytes.
 * @param sectorsPerHunk Number of sectors per hunk.
 * @param userDataOffset Offset of user data within each sector.
 * @param totalHunks     Total number of hunks in the CHD file.
 * @param completedBytes Optional atomic counter updated with bytes written.
 *
 * @return true if conversion completed successfully, false on error or user cancellation.
 */
static bool convertSingleThreaded(chd_file* rawChd, const std::string& isoPath, uint32_t hunkSize,
                                  uint32_t rawSectorSize, uint32_t sectorsPerHunk,
                                  uint32_t userDataOffset, uint32_t totalHunks,
                                  std::atomic<size_t>* completedBytes) {
    std::ofstream isoFile(isoPath, std::ios::binary);
    if (!isoFile.is_open()) return false;
    std::vector<char> writeBuf(1024 * 1024);
    isoFile.rdbuf()->pubsetbuf(writeBuf.data(), writeBuf.size());

    std::vector<uint8_t> hunkBuffer(hunkSize);
    const uint32_t userDataSize = 2048;
    const uint32_t userDataPerHunk = sectorsPerHunk * userDataSize;
    std::vector<uint8_t> hunkUserData(userDataPerHunk);

    for (uint32_t hunk = 0; hunk < totalHunks; ++hunk) {
        if (GlobalState::g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(isoPath);
            return false;
        }
        if (chd_read(rawChd, hunk, hunkBuffer.data()) != CHDERR_NONE)
            return false;

        uint8_t* dest = hunkUserData.data();
        for (uint32_t s = 0; s < sectorsPerHunk; ++s) {
            const uint8_t* src = hunkBuffer.data() + (s * rawSectorSize);
            std::memcpy(dest, src + userDataOffset, userDataSize);
            dest += userDataSize;
        }
        isoFile.write(reinterpret_cast<const char*>(hunkUserData.data()), userDataPerHunk);
        if (completedBytes)
            completedBytes->fetch_add(userDataPerHunk, std::memory_order_relaxed);
    }
    return true;
}

/**
 * @brief Performs two-threaded, memory-mapped writing for CHD to ISO conversion.
 *
 * Used for large files (> 1 GiB). Pre-allocates and memory-maps the output file,
 * then spawns two independent worker threads. Because libchdr handles are not
 * thread-safe, each thread opens its own independent CHD handle, decompresses
 * its assigned range of hunks in parallel, and writes directly into the
 * memory-mapped file region.
 *
 * @param chdPath        Path to the source CHD file.
 * @param isoPath        Path where the output ISO file will be written.
 * @param hunkSize       Size of each hunk in bytes.
 * @param rawSectorSize  Size of each raw sector in bytes.
 * @param sectorsPerHunk Number of sectors per hunk.
 * @param userDataOffset Offset of user data within each sector.
 * @param totalHunks     Total number of hunks in the CHD file.
 * @param totalUserData  Total calculated size of user data in bytes.
 * @param completedBytes Optional atomic counter updated with bytes written.
 *
 * @return true if conversion completed successfully, false on error or user cancellation.
 */
static bool convertMultiThreaded(const std::string& chdPath, const std::string& isoPath,
                                 uint32_t hunkSize, uint32_t rawSectorSize, uint32_t sectorsPerHunk,
                                 uint32_t userDataOffset, uint32_t totalHunks, uint64_t totalUserData,
                                 std::atomic<size_t>* completedBytes) {
    int fd = ::open(isoPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) return false;
    if (::ftruncate(fd, totalUserData) == -1) {
        ::close(fd);
        fs::remove(isoPath);
        return false;
    }
    void* mapped = ::mmap(nullptr, totalUserData, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (mapped == MAP_FAILED) {
        fs::remove(isoPath);
        return false;
    }
    auto unmap = [&] { ::munmap(mapped, totalUserData); };

    const unsigned int numThreads = 2;
    std::vector<std::thread> threads;
    std::atomic<bool> errorOccurred = false;
    uint32_t hunksPerThread = (totalHunks + numThreads - 1) / numThreads;
    const uint32_t userDataSize = 2048;
    const uint32_t userDataPerHunk = sectorsPerHunk * userDataSize;

    for (unsigned int t = 0; t < numThreads; ++t) {
        uint32_t start = t * hunksPerThread;
        uint32_t end = std::min(start + hunksPerThread, totalHunks);
        if (start >= end) continue;

        threads.emplace_back([&, start, end]() {
            chd_file* threadChd = nullptr;
            if (chd_open(chdPath.c_str(), CHD_OPEN_READ, nullptr, &threadChd) != CHDERR_NONE) {
                errorOccurred = true;
                return;
            }
            ChdFilePtr threadChdPtr(threadChd);

            std::vector<uint8_t> hunkBuffer(hunkSize);
            std::vector<uint8_t> userDataBuffer(userDataPerHunk);

            for (uint32_t hunk = start; hunk < end && !errorOccurred; ++hunk) {
                if (GlobalState::g_operationCancelled.load()) {
                    errorOccurred = true;
                    return;
                }
                if (chd_read(threadChd, hunk, hunkBuffer.data()) != CHDERR_NONE) {
                    errorOccurred = true;
                    return;
                }
                uint8_t* dest = userDataBuffer.data();
                for (uint32_t s = 0; s < sectorsPerHunk; ++s) {
                    const uint8_t* src = hunkBuffer.data() + (s * rawSectorSize);
                    std::memcpy(dest, src + userDataOffset, userDataSize);
                    dest += userDataSize;
                }
                uint64_t offset = static_cast<uint64_t>(hunk) * userDataPerHunk;
                std::memcpy(static_cast<char*>(mapped) + offset, userDataBuffer.data(), userDataPerHunk);
                if (completedBytes)
                    completedBytes->fetch_add(userDataPerHunk, std::memory_order_relaxed);
            }
        });
    }

    for (auto& th : threads) {
        if (th.joinable()) th.join();
    }
    unmap();

    if (errorOccurred || GlobalState::g_operationCancelled.load()) {
        fs::remove(isoPath);
        return false;
    }
    return true;
}

/**
 * @brief Converts a CHD (Compressed Hunks of Data) file to a raw ISO image.
 *
 * This function handles multiple CD-ROM sector layouts commonly found in CHD files:
 * - 2048 bytes/sector: Standard ISO (user data only, offset 0)
 * - 2352 bytes/sector: Raw Mode 1 (offset 16) or Mode 2 Form 1 (offset 24)
 * - 2448 bytes/sector: Raw with subchannel data (legacy offset 24) or cooked (modern offset 0)
 *
 * Detection is performed by reading logical sector 16 (the ISO9660 Primary Volume
 * Descriptor) and locating the "CD001" signature to determine the correct user
 * data offset for this specific image. This unified approach works for both
 * legacy CHDs (created with older tools) and modern CHDs (e.g., from chdman).
 *
 * Conversion strategy is chosen based on the output ISO size:
 * - For files leq 1 GiB: a single-threaded, buffered write is used (low overhead).
 * - For files > 1 GiB: two threads decompress hunks in parallel, writing directly
 *   into a memory-mapped output file. This provides a significant speedup without
 *   saturating all CPU cores, leaving system resources available for other tasks.
 *
 * @param chdPath        Path to the source CHD file.
 * @param isoPath        Path where the output ISO file will be written.
 * @param completedBytes Optional atomic counter updated with the number of
 *                       user data bytes written so far. Useful for progress
 *                       reporting. May be nullptr.
 *
 * @return true if conversion completed successfully, false on error or cancellation.
 *
 * @note The conversion can be cancelled by setting GlobalState::g_operationCancelled to true.
 *       If cancelled, the partial ISO file is removed.
 *
 * @note For large files, two independent CHD handles are opened internally;
 *       the original handle is used only for header reading and offset detection.
 *
 * @see GlobalState::g_operationCancelled Global cancellation flag.
 */
bool convertChdToIso(const std::string& chdPath, const std::string& isoPath,
                     std::atomic<size_t>* completedBytes) {
    if (GlobalState::g_operationCancelled.load()) return false;

    chd_file* rawChd = nullptr;
    if (chd_open(chdPath.c_str(), CHD_OPEN_READ, nullptr, &rawChd) != CHDERR_NONE)
        return false;
    ChdFilePtr chd(rawChd);
    const chd_header* header = chd_get_header(chd.get());
    if (!header) return false;

    uint32_t hunkSize = header->hunkbytes;
    uint32_t rawSectorSize = 0;
    if (hunkSize % 2448 == 0)      rawSectorSize = 2448;
    else if (hunkSize % 2352 == 0) rawSectorSize = 2352;
    else if (hunkSize % 2048 == 0) rawSectorSize = 2048;
    else return false;

    uint32_t sectorsPerHunk = hunkSize / rawSectorSize;
    const uint32_t userDataSize = 2048;
    const uint32_t userDataPerHunk = sectorsPerHunk * userDataSize;
    const uint64_t totalUserData = static_cast<uint64_t>(header->totalhunks) * userDataPerHunk;

    uint32_t userDataOffset = detectUserDataOffset(chd.get(), hunkSize, rawSectorSize, sectorsPerHunk);

    const uint64_t ONE_GB = 1ULL << 30;
    if (totalUserData <= ONE_GB) {
        return convertSingleThreaded(chd.get(), isoPath, hunkSize, rawSectorSize,
                                     sectorsPerHunk, userDataOffset, header->totalhunks, completedBytes);
    }

    return convertMultiThreaded(chdPath, isoPath, hunkSize, rawSectorSize,
                                  sectorsPerHunk, userDataOffset, header->totalhunks,
                                  totalUserData, completedBytes);
}
