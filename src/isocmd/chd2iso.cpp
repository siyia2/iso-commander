// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../chd.h"

/**
 * @brief Detect user data offset for 2352-byte sectors by checking ISO volume descriptor
 * 
 * @param chd CHD file handle
 * @param header CHD header information
 * @param rawSectorSize Raw sector size (must be 2352)
 * @return uint32_t Detected offset (16, 24, or fallback 16)
 */
static uint32_t detectUserDataOffset(chd_file* chd, const chd_header* header, uint32_t rawSectorSize) {
    if (rawSectorSize != 2352) return 0;

    uint32_t hunkSize = header->hunkbytes;
    std::vector<uint8_t> hunkBuffer(hunkSize);
    if (chd_read(chd, 0, hunkBuffer.data()) != CHDERR_NONE)
        return 16;

    const uint8_t* firstSector = hunkBuffer.data();
    if (std::memcmp(firstSector + 16, "CD001", 5) == 0)
        return 16;
    if (std::memcmp(firstSector + 24, "CD001", 5) == 0)
        return 24;
    return 16;
}

/**
 * @brief Convert a CHD disc image to a raw ISO 9660 image
 * 
 * Supports CD-ROM (2352/2448 bytes) and pure data (2048 bytes) sectors.
 * Automatically detects correct user data offset for 2352-byte sectors.
 * 
 * @param chdPath Input .chd file path
 * @param isoPath Output .iso file path
 * @param completedBytes Optional atomic counter for progress tracking
 * @return true on success, false on error or cancellation
 */
bool convertChdToIso(const std::string& chdPath, const std::string& isoPath,
                     std::atomic<size_t>* completedBytes) {
    if (g_operationCancelled.load()) return false;

    chd_file* rawChd = nullptr;
    chd_error err = chd_open(chdPath.c_str(), CHD_OPEN_READ, nullptr, &rawChd);
    if (err != CHDERR_NONE) return false;
    ChdFilePtr chd(rawChd);

    const chd_header* header = chd_get_header(chd.get());
    if (!header) return false;

    uint32_t hunkSize = header->hunkbytes;
    uint32_t rawSectorSize = 0;
    if (hunkSize % 2448 == 0) rawSectorSize = 2448;
    else if (hunkSize % 2352 == 0) rawSectorSize = 2352;
    else if (hunkSize % 2048 == 0) rawSectorSize = 2048;
    else return false;

    uint32_t userDataOffset = 0;
    uint32_t userDataSize = 2048;
    if (rawSectorSize == 2448) {
        userDataOffset = 24;
    } else if (rawSectorSize == 2352) {
        userDataOffset = detectUserDataOffset(chd.get(), header, rawSectorSize);
    } else {
        userDataOffset = 0;
    }

    uint32_t sectorsPerHunk = hunkSize / rawSectorSize;

    if (g_operationCancelled.load()) return false;

    std::ofstream isoFile(isoPath, std::ios::binary);
    if (!isoFile.is_open()) return false;

    size_t outBufSize = 4 * 1024 * 1024;
    std::vector<char> outBuffer(outBufSize);
    isoFile.rdbuf()->pubsetbuf(outBuffer.data(), outBufSize);

    std::vector<uint8_t> hunkBuffer(hunkSize);
    std::vector<uint8_t> hunkUserData(sectorsPerHunk * userDataSize);

    if (completedBytes) completedBytes->store(0);

    for (uint32_t hunk = 0; hunk < header->totalhunks; ++hunk) {
        if (g_operationCancelled.load()) {
            isoFile.close();
            std::error_code ec;
            fs::remove(isoPath, ec);
            return false;
        }

        err = chd_read(chd.get(), hunk, hunkBuffer.data());
        if (err != CHDERR_NONE) return false;

        uint8_t* dest = hunkUserData.data();
        for (uint32_t s = 0; s < sectorsPerHunk; ++s) {
            const uint8_t* rawSector = hunkBuffer.data() + s * rawSectorSize;
            std::memcpy(dest, rawSector + userDataOffset, userDataSize);
            dest += userDataSize;
        }

        size_t bytesToWrite = sectorsPerHunk * userDataSize;
        if (!isoFile.write(reinterpret_cast<const char*>(hunkUserData.data()), bytesToWrite))
            return false;

        if (completedBytes)
            completedBytes->fetch_add(bytesToWrite, std::memory_order_relaxed);
    }

    return true;
}
