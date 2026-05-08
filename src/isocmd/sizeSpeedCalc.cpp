// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <cstdio>
#include <fstream>
#include <vector>
#include <iomanip>
#include <sstream>
#include <string>

// C / System Headers
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>

// Project Headers
#include "../ccd.h"
#include "../daa2iso.h"
#include "../mdf.h"
#include "../write2usb.h"
#include "chd.h"

//=============================================================================
// Write2Usb Section
//=============================================================================

/**
 * @brief Queries the size of a block device in bytes.
 *
 * Attempts @c BLKGETSIZE64 first (returns bytes directly), then falls back to
 * @c BLKGETSIZE (returns 512-byte sector count) if the first ioctl fails.
 *
 * @param device Absolute path to the block device (e.g. @c /dev/sdb).
 * @return Size in bytes, or @c 0 on failure or if the device cannot be opened.
 */
uint64_t getBlockDeviceSize(const std::string& device) {
    int fd = open(device.c_str(), O_RDONLY);
    if (fd == -1) {
        return 0;
    }

    uint64_t size = 0;
    
    if (ioctl(fd, BLKGETSIZE64, &size) == 0) {
        close(fd);
        return size;
    }
    
    unsigned long sectors = 0;
    if (ioctl(fd, BLKGETSIZE, &sectors) == 0) {
        close(fd);
        return sectors * 512ULL;
    }
    
    close(fd);
    return 0;
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

    for (const auto& file : filesToProcess) {
        std::string ext = file.substr(file.find_last_of(".") + 1);
        toLowerInPlace(ext);

        if (modeChd && ext == "chd") {
            chd_file* rawChd = nullptr;
            chd_error err = chd_open(file.c_str(), CHD_OPEN_READ, nullptr, &rawChd);
            if (err == CHDERR_NONE && rawChd) {
                const chd_header* header = chd_get_header(rawChd);
                if (header) {
                    uint32_t hunkSize = header->hunkbytes;
                    uint32_t rawSectorSize = 0;
                    if      (hunkSize % 2448 == 0) rawSectorSize = 2448;
                    else if (hunkSize % 2352 == 0) rawSectorSize = 2352;
                    else if (hunkSize % 2048 == 0) rawSectorSize = 2048;
                    if (rawSectorSize != 0) {
                        constexpr uint32_t userDataSize = 2048;
                        uint32_t sectorsPerHunk = hunkSize / rawSectorSize;
                        uint64_t totalSectors = static_cast<uint64_t>(header->totalhunks) * sectorsPerHunk;
                        totalBytes += totalSectors * userDataSize;
                    }
                }
                chd_close(rawChd);
            }
        }
        else if (modeNrg && ext == "nrg") {
            std::ifstream nrg(file, std::ios::binary | std::ios::ate);
            if (nrg) {
                std::streampos pos = nrg.tellg();
                if (pos > 0) {
                    size_t sz = static_cast<size_t>(pos);
                    totalBytes += (sz > 307200) ? (sz - 307200) : 0;
                }
            }
        }
        else if (modeMdf && ext == "mdf") {
            std::ifstream mdf(file, std::ios::binary);
            if (mdf) {
                MdfTypeInfo info;
                if (!info.determineMdfType(mdf)) continue;
                mdf.seekg(0, std::ios::end);
                std::streampos pos = mdf.tellg();
                if (pos < 0) continue;
                size_t fileSize = static_cast<size_t>(pos);
                size_t sectors = fileSize / info.sector_size;
                totalBytes += sectors * info.sector_data;
            }
        }
        else if (modeDaa && (ext == "daa" || ext == "gbi")) {
            uint64_t isoSize = getDaaIsoSize(file);
            if (isoSize > 0) totalBytes += isoSize;
        }
        else if (!modeMdf && !modeNrg && !modeChd && !modeDaa &&
                 (ext == "bin" || ext == "img" || ext == "ccd")) {
            std::ifstream ccd(file, std::ios::binary | std::ios::ate);
            if (ccd) {
                std::streampos pos = ccd.tellg();
                if (pos > 0)
                    totalBytes += (static_cast<size_t>(pos) / sizeof(CcdSector)) * DATA_SIZE;
            }
        }
    }

    return totalBytes;
}
