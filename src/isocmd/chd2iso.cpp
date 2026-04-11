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
    uint32_t userDataOffset = 0;

    // --- DETECT OFFSET BY READING SECTOR 16 (ISO9660 PVD) ---
    if (rawSectorSize == 2048) {
        userDataOffset = 0;
    } else {
        uint32_t targetHunk = 16 / sectorsPerHunk;
        uint32_t sectorIndex = 16 % sectorsPerHunk;
        std::vector<uint8_t> testBuf(hunkSize);
        bool detected = false;

        if (chd_read(chd.get(), targetHunk, testBuf.data()) == CHDERR_NONE) {
            const uint8_t* s16 = testBuf.data() + (sectorIndex * rawSectorSize);

            // Candidate user data offsets for each raw sector size
            std::vector<uint32_t> candidates;
            if (rawSectorSize == 2352) {
                candidates = {16, 24};      // Mode 1 / Mode 2 Form 1
            } else if (rawSectorSize == 2448) {
                candidates = {0, 16, 24};   // Cooked / Mode1+sub / Mode2+sub
            }

            for (uint32_t off : candidates) {
                // Signature "CD001" starts at byte 1 of the user data block
                if (std::memcmp(s16 + off + 1, "CD001", 5) == 0) {
                    userDataOffset = off;
                    detected = true;
                    break;
                }
            }
        }

        if (!detected) {
            // Fallback: most common offsets for each type
            userDataOffset = (rawSectorSize == 2352) ? 16 : 0;
        }
    }

    // --- CONVERSION LOOP (unchanged) ---
    const uint32_t userDataSize = 2048;
    std::ofstream isoFile(isoPath, std::ios::binary);
    if (!isoFile.is_open()) return false;

    std::vector<char> writeBuf(1024 * 1024);
    isoFile.rdbuf()->pubsetbuf(writeBuf.data(), writeBuf.size());

    std::vector<uint8_t> hunkBuffer(hunkSize);
    std::vector<uint8_t> hunkUserData(sectorsPerHunk * userDataSize);

    if (completedBytes) completedBytes->store(0);

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

        size_t totalBytes = sectorsPerHunk * userDataSize;
        isoFile.write(reinterpret_cast<const char*>(hunkUserData.data()), totalBytes);

        if (completedBytes)
            completedBytes->fetch_add(totalBytes, std::memory_order_relaxed);
    }

    return true;
}
