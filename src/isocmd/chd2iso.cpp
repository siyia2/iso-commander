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
    if (g_operationCancelled.load()) return false;

    chd_file* rawChd = nullptr;
    chd_error err = chd_open(chdPath.c_str(), CHD_OPEN_READ, nullptr, &rawChd);
    if (err != CHDERR_NONE) return false;
    ChdFilePtr chd(rawChd);

    const chd_header* header = chd_get_header(chd.get());
    if (!header) return false;

    // Determine raw sector size
    const uint32_t hunkSize = header->hunkbytes;
    uint32_t rawSectorSize = 0;
    if (hunkSize % 2448 == 0) rawSectorSize = 2448;
    else if (hunkSize % 2352 == 0) rawSectorSize = 2352;
    else if (hunkSize % 2048 == 0) rawSectorSize = 2048;
    else return false;

    // User data offset
    uint32_t userDataOffset = 0;
    const uint32_t userDataSize = 2048;
    if (rawSectorSize == 2448) userDataOffset = 24;
    else if (rawSectorSize == 2352) userDataOffset = 16;

    const uint32_t sectorsPerHunk = hunkSize / rawSectorSize;

    if (g_operationCancelled.load()) return false;

    std::ofstream isoFile(isoPath, std::ios::binary);
    if (!isoFile.is_open()) return false;

    // Increase output buffer size (1 MiB)
    const size_t outBufSize = 1024 * 1024;
    std::vector<char> outBuffer(outBufSize);
    isoFile.rdbuf()->pubsetbuf(outBuffer.data(), outBufSize);

    // Buffers
    std::vector<uint8_t> hunkBuffer(hunkSize);
    std::vector<uint8_t> hunkUserData(sectorsPerHunk * userDataSize);

    if (completedBytes) completedBytes->store(0);

    for (uint32_t hunk = 0; hunk < header->totalhunks; ++hunk) {
        if (g_operationCancelled.load()) {
            isoFile.close();
            std::error_code ec;
            std::filesystem::remove(isoPath, ec);
            return false;
        }

        err = chd_read(chd.get(), hunk, hunkBuffer.data());
        if (err != CHDERR_NONE) return false;

        // Extract user data from all sectors in this hunk
        uint8_t* dest = hunkUserData.data();
        for (uint32_t s = 0; s < sectorsPerHunk; ++s) {
            const uint8_t* rawSector = hunkBuffer.data() + s * rawSectorSize;
            std::memcpy(dest, rawSector + userDataOffset, userDataSize);
            dest += userDataSize;
        }

        // Write the whole hunk's worth of user data at once
        const size_t bytesToWrite = sectorsPerHunk * userDataSize;
        if (!isoFile.write(reinterpret_cast<const char*>(hunkUserData.data()), bytesToWrite))
            return false;

        if (completedBytes)
            completedBytes->fetch_add(bytesToWrite, std::memory_order_relaxed);
    }

    return true;
}
