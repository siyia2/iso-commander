/* SPDX-License-Identifier: BSD-3-Clause */

#include "../headers.h"
#include "../chd.h"

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
 * - For files ≤ 1 GiB: a single‑threaded, buffered write is used (low overhead).
 * - For files > 1 GiB: two threads decompress hunks in parallel, writing directly
 *   into a memory‑mapped output file. This provides a significant speedup without
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
 * @note The conversion can be cancelled by setting g_operationCancelled to true.
 *       If cancelled, the partial ISO file is removed.
 * 
 * @note For large files, two independent CHD handles are opened internally;
 *       the original handle is used only for header reading and offset detection.
 * 
 * @see g_operationCancelled Global cancellation flag.
 */
bool convertChdToIso(const std::string& chdPath, const std::string& isoPath,
                     std::atomic<size_t>* completedBytes) {
    if (g_operationCancelled.load()) return false;

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

    // --- Detect user data offset ---
    uint32_t userDataOffset = 0;
    if (rawSectorSize == 2048) {
        userDataOffset = 0;
    } else {
        uint32_t targetHunk = 16 / sectorsPerHunk;
        uint32_t sectorIndex = 16 % sectorsPerHunk;
        std::vector<uint8_t> testBuf(hunkSize);
        bool detected = false;
        if (chd_read(chd.get(), targetHunk, testBuf.data()) == CHDERR_NONE) {
            const uint8_t* s16 = testBuf.data() + (sectorIndex * rawSectorSize);
            std::vector<uint32_t> candidates;
            if (rawSectorSize == 2352) {
                candidates = {16, 24};
            } else if (rawSectorSize == 2448) {
                candidates = {0, 16, 24};
            }
            for (uint32_t off : candidates) {
                if (std::memcmp(s16 + off + 1, "CD001", 5) == 0) {
                    userDataOffset = off;
                    detected = true;
                    break;
                }
            }
        }
        if (!detected) {
            userDataOffset = (rawSectorSize == 2352) ? 16 : 0;
        }
    }

    // --- Decide strategy based on file size ---
    const uint64_t ONE_GB = 1ULL << 30;
    if (totalUserData <= ONE_GB) {
        // ---------- SINGLE‑THREADED (original path) ----------
        std::ofstream isoFile(isoPath, std::ios::binary);
        if (!isoFile.is_open()) return false;
        std::vector<char> writeBuf(1024 * 1024);
        isoFile.rdbuf()->pubsetbuf(writeBuf.data(), writeBuf.size());

        std::vector<uint8_t> hunkBuffer(hunkSize);
        std::vector<uint8_t> hunkUserData(userDataPerHunk);

        for (uint32_t hunk = 0; hunk < header->totalhunks; ++hunk) {
            if (g_operationCancelled.load()) {
                isoFile.close();
                fs::remove(isoPath);
                return false;
            }
            if (chd_read(chd.get(), hunk, hunkBuffer.data()) != CHDERR_NONE)
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

    // ---------- LARGE FILE (>1GB) : 2‑THREAD PARALLEL with memory mapping ----------
    // Pre‑allocate and memory‑map the output file
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

    uint32_t totalHunks = header->totalhunks;
    uint32_t hunksPerThread = (totalHunks + numThreads - 1) / numThreads;

    for (unsigned int t = 0; t < numThreads; ++t) {
        uint32_t start = t * hunksPerThread;
        uint32_t end = std::min(start + hunksPerThread, totalHunks);
        if (start >= end) continue;

        threads.emplace_back([&, start, end]() {
            // Each thread opens its own CHD handle (libchdr is not thread‑safe otherwise)
            chd_file* threadChd = nullptr;
            if (chd_open(chdPath.c_str(), CHD_OPEN_READ, nullptr, &threadChd) != CHDERR_NONE) {
                errorOccurred = true;
                return;
            }
            ChdFilePtr threadChdPtr(threadChd);

            std::vector<uint8_t> hunkBuffer(hunkSize);
            std::vector<uint8_t> userDataBuffer(userDataPerHunk);

            for (uint32_t hunk = start; hunk < end && !errorOccurred; ++hunk) {
                if (g_operationCancelled.load()) {
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

    if (errorOccurred || g_operationCancelled.load()) {
        fs::remove(isoPath);
        return false;
    }
    return true;
}
