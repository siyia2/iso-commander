// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

// C / System Headers
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

// Third-Party Library Headers
#include "chd.h"

// Project Headers
#include "../ccd.h"
#include "../daa2iso.h"
#include "../mdf.h"
#include "../write2usb.h"

//=============================================================================
// Write2Usb Section
//=============================================================================

/**
 * @brief Queries the size of a block device in bytes.
 *
 * Reads @c /sys/block/<name>/size from sysfs, which reports the device
 * size in 512-byte logical sectors. This avoids opening the block device
 * directly, preventing thread blocking during active kernel I/O flushes.
 *
 * @param device Absolute path to the block device (e.g. @c /dev/sdb).
 * @return Size in bytes, or @c 0 on failure or if the sysfs entry is absent.
 */
uint64_t getBlockDeviceSize(const std::string& device) {
    std::string deviceName = device.substr(device.find_last_of('/') + 1);
    std::ifstream sizeFile("/sys/block/" + deviceName + "/size");

    uint64_t sectors = 0;
    if (!(sizeFile >> sectors)) {
        return 0;
    }

    // sysfs /size is always in 512-byte logical sectors regardless of
    // the drive's physical sector size
    return sectors * 512ULL;
}

/**
 * @brief Formats a byte count as a human-readable size string.
 *
 * Produces output in KB, MB, or GB with two decimal places depending on
 * the magnitude of @p size.
 *
 * @param size Raw size in bytes.
 * @return Formatted string such as @c "4.70 GB" or @c "720.00 MB".
 */
std::string formatFileSize(uint64_t size) {
    std::ostringstream oss;
    if (size < 1024 * 1024) {
        oss << std::fixed << std::setprecision(2)
            << static_cast<double>(size) / 1024 << " KB";
    } else if (size < 1024 * 1024 * 1024) {
        oss << std::fixed << std::setprecision(2)
            << static_cast<double>(size) / (1024 * 1024) << " MB";
    } else {
        oss << std::fixed << std::setprecision(2)
            << static_cast<double>(size) / (1024 * 1024 * 1024) << " GB";
    }
    return oss.str();
}

/**
 * @brief Formats a write speed as a human-readable string.
 *
 * Returns KB/s for speeds below 0.1 MB/s, otherwise MB/s, both with one
 * decimal place.
 *
 * @param mbPerSec Write speed in megabytes per second.
 * @return Formatted string such as @c "45.3 MB/s" or @c "98.4 KB/s".
 */
std::string formatSpeed(double mbPerSec) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    if (mbPerSec < 0.1) {
        oss << (mbPerSec * 1024) << " KB/s";
    } else {
        oss << mbPerSec << " MB/s";
    }
    return oss.str();
}

//=============================================================================
// Convert2ISO Section
//=============================================================================

/**
 * @brief Get the uncompressed ISO size from a DAA file
 *
 * Reads the DAA file header and extracts the original ISO image size
 * before compression. This is useful for pre-allocating buffers or
 * verifying available disk space before extraction.
 *
 * @param path Path to the DAA file (UTF-8 encoded on applicable platforms)
 * @return Uncompressed ISO size in bytes, or 0 if:
 *         - File cannot be opened
 *         - Read operation fails
 *         - DAA signature is invalid (not "DAA" or "GBI")
 *
 * @note The function automatically handles endianness detection and
 *       byte-swapping based on the host platform.
 *
 * @warning The returned size is the claimed size from the DAA header;
 *          no validation is performed against actual compressed data.
 *
 * @see daa_t::isosize
 */
static uint64_t getDaaIsoSize(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;

    daa_t daa;
    if (fread(&daa, 1, sizeof(daa), f) != sizeof(daa)) {
        fclose(f);
        return 0;
    }
    fclose(f);

    // Determine host endianness (0 = little-endian, no swap)
    int endian = 1;
    if (*(char*)&endian) endian = 0;
    swap_daa_if_be(&daa, endian);

    // Check signature (basic validation)
    if (strncmp((char*)daa.sign, "DAA", 16) != 0 &&
        strncmp((char*)daa.sign, "GBI", 16) != 0) {
        return 0;
    }
    return daa.isosize;
}


/**
 * @brief Sums the physical file sizes of a given list of file paths.
 * * @param files Vector of file paths.
 * @return Total size in bytes.
 */
size_t getTotalFileSize(const std::vector<std::string>& files) {
    size_t totalSize = 0;
    for (const auto& file : files) {
        struct stat st;
        if (stat(file.c_str(), &st) == 0) {
            totalSize += st.st_size;
        }
    }
    return totalSize;
}

void toLowerInPlace(std::string& str);

/**
 * @brief Estimates the uncompressed ISO size for a Compressed Hunks Data (CHD) image file.
 *
 * Opens the CHD header via libchdr, detects the sector geometry (2448, 2352, or 2048 bytes),
 * and computes the total sector count multiplied by the standard 2048-byte user data size.
 *
 * @param file Absolute path to the CHD file.
 * @return Estimated size in bytes of the resulting ISO, or 0 if opening/parsing fails.
 */
static size_t estimateChdBytes(const std::string& file) {
    chd_file* rawChd = nullptr;
    if (chd_open(file.c_str(), CHD_OPEN_READ, nullptr, &rawChd) != CHDERR_NONE || !rawChd) {
        return 0;
    }

    size_t bytes = 0;
    if (const chd_header* header = chd_get_header(rawChd)) {
        uint32_t hunkSize = header->hunkbytes;
        uint32_t rawSectorSize = 0;
        if      (hunkSize % 2448 == 0) rawSectorSize = 2448;
        else if (hunkSize % 2352 == 0) rawSectorSize = 2352;
        else if (hunkSize % 2048 == 0) rawSectorSize = 2048;

        if (rawSectorSize != 0) {
            constexpr uint32_t userDataSize = 2048;
            uint32_t sectorsPerHunk = hunkSize / rawSectorSize;
            uint64_t totalSectors = static_cast<uint64_t>(header->totalhunks) * sectorsPerHunk;
            bytes = totalSectors * userDataSize;
        }
    }
    chd_close(rawChd);
    return bytes;
}

/**
 * @brief Estimates the output ISO size for a Nero Burning ROM (NRG) image file.
 *
 * Computes the size by reading the raw file length minus a fixed 300 KB
 * (307,200 bytes) header/footer offset typical of NRG container formats.
 *
 * @param file Absolute path to the NRG file.
 * @return Estimated size in bytes of the resulting ISO, or 0 if the file cannot be read.
 */
static size_t estimateNrgBytes(const std::string& file) {
    std::ifstream nrg(file, std::ios::binary | std::ios::ate);
    if (!nrg) return 0;
    std::streampos pos = nrg.tellg();
    if (pos <= 0) return 0;
    size_t sz = static_cast<size_t>(pos);
    return (sz > 307200) ? (sz - 307200) : 0;
}

/**
 * @brief Estimates the output ISO size for a Media Descriptor Image (MDF/MDS) file.
 *
 * Detects sector geometry using @c MdfTypeInfo and calculates total sector count
 * multiplied by the specific sector data payload size.
 *
 * @param file Absolute path to the MDF file.
 * @return Estimated size in bytes of the resulting ISO, or 0 if geometry is undetectable.
 */
static size_t estimateMdfBytes(const std::string& file) {
    std::ifstream mdf(file, std::ios::binary);
    if (!mdf) return 0;
    MdfTypeInfo info;
    if (!info.determineMdfType(mdf)) return 0;
    mdf.seekg(0, std::ios::end);
    std::streampos pos = mdf.tellg();
    if (pos < 0) return 0;
    size_t fileSize = static_cast<size_t>(pos);
    return (fileSize / info.sector_size) * info.sector_data;
}

/**
 * @brief Estimates the output ISO size for a Direct Access Archive (DAA/GBI) image file.
 *
 * Delegates size resolution to the underlying @c getDaaIsoSize helper routine.
 *
 * @param file Absolute path to the DAA or GBI file.
 * @return Uncompressed ISO size in bytes, or 0 if extraction fails.
 */
static size_t estimateDaaBytes(const std::string& file) {
    uint64_t isoSize = getDaaIsoSize(file);
    return (isoSize > 0) ? static_cast<size_t>(isoSize) : 0;
}

/**
 * @brief Estimates the output ISO size for raw disc image formats (BIN, IMG, CCD).
 *
 * Computes total file sectors based on @c CcdSector geometry and scales them
 * by standard @c DATA_SIZE bytes.
 *
 * @param file Absolute path to the raw image file.
 * @return Estimated size in bytes of the resulting ISO, or 0 on read failure.
 */
static size_t estimateBinBytes(const std::string& file) {
    std::ifstream ccd(file, std::ios::binary | std::ios::ate);
    if (!ccd) return 0;
    std::streampos pos = ccd.tellg();
    if (pos <= 0) return 0;
    return (static_cast<size_t>(pos) / sizeof(CcdSector)) * DATA_SIZE;
}

/**
 * @brief Calculate the total estimated output size in bytes for a batch of disc image conversions.
 *
 * Iterates over the provided file list and estimates the size of the resulting ISO for each file
 * based on the active conversion mode. Each format is handled according to its sector geometry:
 *
 * - **CHD**: Opens the CHD header via libchdr, detects sector size (2448/2352/2048 bytes),
 *   and computes total sectors × 2048 bytes user data, mirroring convertChdToIso().
 * - **NRG**: Raw file size minus a fixed 300 KB header/footer offset.
 * - **MDF**: Detects sector geometry via MdfTypeInfo, computes sectors × sector_data,
 *   mirroring convertMdfToIso().
 * - **DAA**: Delegates to getDaaIsoSize() for the uncompressed ISO size.
 * - **BIN/IMG/CCD**: Computes (file size / CcdSector size) × DATA_SIZE user bytes.
 *
 * Files with unsupported or undetectable geometry are silently skipped.
 * The result is used to initialize the progress bar's total byte target.
 *
 * @param filesToProcess List of absolute paths to disc image files to be converted.
 * @param modeMdf        True if converting MDF/MDS images.
 * @param modeNrg        True if converting NRG images.
 * @param modeChd        True if converting CHD images.
 * @param modeDaa        True if converting DAA images.
 * @return Total estimated output size in bytes across all files.
 */
size_t calculateTotalBytesForConversions(
    const std::vector<std::string>& filesToProcess,
    const bool modeMdf, const bool modeNrg,
    const bool modeChd, const bool modeDaa)
{
    size_t totalBytes = 0;
    const bool noModesActive = !modeMdf && !modeNrg && !modeChd && !modeDaa;

    for (const auto& file : filesToProcess) {
        std::string ext = file.substr(file.find_last_of(".") + 1);
        toLowerInPlace(ext);

        if (modeChd && ext == "chd") {
            totalBytes += estimateChdBytes(file);
        }
        else if (modeNrg && ext == "nrg") {
            totalBytes += estimateNrgBytes(file);
        }
        else if (modeMdf && ext == "mdf") {
            totalBytes += estimateMdfBytes(file);
        }
        else if (modeDaa && (ext == "daa" || ext == "gbi")) {
            totalBytes += estimateDaaBytes(file);
        }
        else if (noModesActive && (ext == "bin" || ext == "img" || ext == "ccd")) {
            totalBytes += estimateBinBytes(file);
        }
    }

    return totalBytes;
}
