#include "../headers.h"
#include "../chd.h"

/**
 * Convert a CHD disc image (CD/DVD) to a raw ISO 9660 image.
 *
 * @param chdPath      Path to the input .chd file.
 * @param isoPath      Path where the output .iso file will be written.
 * @param completedBytes Optional atomic counter updated with written bytes (2048‑byte sectors).
 * @return true on success, false on error or cancellation.
 */
bool convertChdToIso(const std::string& chdPath, const std::string& isoPath,
                     std::atomic<size_t>* completedBytes) {
    // Early cancellation check
    if (g_operationCancelled.load()) {
        g_operationCancelled.store(true);
        return false;
    }

    // 1. Open CHD file using libchdr
    chd_file* rawChd = nullptr;
    chd_error err = chd_open(chdPath.c_str(), CHD_OPEN_READ, nullptr, &rawChd);
    if (err != CHDERR_NONE) {
        return false;
    }
    ChdFilePtr chd(rawChd);

    // 2. Get CHD header information
    const chd_header* header = chd_get_header(chd.get());
    if (!header) {
        return false;
    }

    // 3. Determine sector layout
    const uint32_t hunkSize = header->hunkbytes;
    uint32_t rawSectorSize = 0;
    uint32_t userDataOffset = 0;
    const uint32_t userDataSize = 2048;   // ISO 9660 sector size

    if (hunkSize % 2352 == 0) {
        rawSectorSize = 2352;          // CD‑ROM raw sector
        userDataOffset = 16;           // skip sync + header to get user data
    } else if (hunkSize % 2048 == 0) {
        rawSectorSize = 2048;          // already pure data
        userDataOffset = 0;
    } else {
        return false;                  // unsupported sector size
    }

    const uint32_t sectorsPerHunk = hunkSize / rawSectorSize;

    // 4. Prepare output ISO file
    if (g_operationCancelled.load()) {
        g_operationCancelled.store(true);
        return false;
    }

    std::ofstream isoFile(isoPath, std::ios::binary);
    if (!isoFile.is_open()) {
        return false;
    }

    // 5. Allocate buffers
    std::vector<uint8_t> hunkBuffer(hunkSize);
    std::vector<uint8_t> sectorBuffer(userDataSize);

    // 6. Progress initialisation
    if (completedBytes) {
        completedBytes->store(0);
    }

    // 7. Main conversion loop
    for (uint32_t hunk = 0; hunk < header->totalhunks; ++hunk) {
        if (g_operationCancelled.load()) {
            isoFile.close();
            std::error_code ec;
            std::filesystem::remove(isoPath, ec);
            g_operationCancelled.store(true);
            return false;
        }

        // Read the whole hunk (decompressed by libchdr)
        err = chd_read(chd.get(), hunk, hunkBuffer.data());
        if (err != CHDERR_NONE) {
            return false;
        }

        // Process each sector inside this hunk
        for (uint32_t s = 0; s < sectorsPerHunk; ++s) {
            const uint8_t* rawSector = hunkBuffer.data() + s * rawSectorSize;
            std::memcpy(sectorBuffer.data(), rawSector + userDataOffset, userDataSize);

            if (!isoFile.write(reinterpret_cast<const char*>(sectorBuffer.data()), userDataSize)) {
                return false;
            }

            if (completedBytes) {
                completedBytes->fetch_add(userDataSize, std::memory_order_relaxed);
            }
        }
    }

    return true;
}
