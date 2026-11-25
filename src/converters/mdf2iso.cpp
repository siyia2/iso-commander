// SPDX-License-Identifier: GPL-3.0-or-later

#include <stddef.h>

import std;
import isocmd.converters.mdf2iso;
import isocmd.core.cancellation;

namespace fs = std::filesystem;

// Special thanks to Salvatore Santagati (mdf2iso).
// Original code modernized and ported to C++.

/*
    Copyright (C) 2004,2005 Salvatore Santagati
    <salvatore.santagati@gmail.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.
*/

/**
 * @brief Detects the MDF sector layout by examining CD-ROM sync patterns.
 *
 * Reads the initial 12-byte sync pattern and, when present, checks
 * the corresponding pattern at offset 2352 to distinguish between
 * 2352-byte and 2448-byte sectors.
 *
 * Supports three layouts:
 * - Type 1: 2352-byte sectors with 2048-byte user data.
 * - Type 2: 2448-byte sectors with 2048-byte user data.
 * - Type 3: 2448-byte sectors with 2352-byte user data.
 *
 * Updates the sector layout members according to the detected format.
 *
 * @param[in,out] mdfFile Open, seekable binary input stream for the MDF image.
 *
 * @return true if the required sync-pattern reads succeed and a layout
 *         is determined; false if either required read fails.
 *
 * @note The input stream position is modified during detection.
 * @note This method detects the layout based on sync patterns and does
 *       not perform comprehensive MDF format validation.
 */
bool MdfTypeInfo::determineMdfType(std::ifstream& mdfFile) {
    static constexpr char kSyncPattern[12] = {
        '\x00', '\xFF', '\xFF', '\xFF', '\xFF', '\xFF',
        '\xFF', '\xFF', '\xFF', '\xFF', '\xFF', '\x00'
    };

    char buf[12];

    mdfFile.seekg(0);
    if (!mdfFile.read(buf, 12)) {
        return false;
    }

    if (std::memcmp(kSyncPattern, buf, 12) == 0) {
        mdfFile.seekg(2352);
        if (!mdfFile.read(buf, 12)) {
            return false;
        }

        seek_head = 16;
        sector_data = 2048;

        if (std::memcmp(kSyncPattern, buf, 12) == 0) {
            // Type 1: 2352-byte sectors with 2048-byte user data.
            seek_ecc = 288;
            sector_size = 2352;
        } else {
            // Type 2: 2448-byte sectors with 2048-byte user data.
            seek_ecc = 384;
            sector_size = 2448;
        }
    } else {
        // Type 3: 2448-byte sectors with 2352-byte user data.
        seek_head = 0;
        sector_size = 2448;
        seek_ecc = 96;
        sector_data = 2352;
    }

    return true;
}

namespace {

    /**
     * @brief Checks and latches the global operation cancellation flag.
     *
     * @return true if cancellation was requested, false otherwise.
     */
    bool consumeCancellationFlag() {
        if (GlobalState::g_operationCancelled.load()) {
            GlobalState::g_operationCancelled.store(true);
            return true;
        }

        return false;
    }

    /**
     * @brief Closes the output ISO stream, deletes any partial file, and marks cancellation.
     *
     * @param[in,out] isoFile Output file stream for the target ISO image.
     * @param[in] isoPath Filesystem path to the target ISO image to remove.
     * @return Always returns false to indicate an aborted conversion.
     */
    bool abortConversion(std::ofstream& isoFile, const std::string& isoPath) {
        isoFile.close();
        fs::remove(isoPath);
        GlobalState::g_operationCancelled.store(true);
        return false;
    }

    /**
     * @brief Evaluates the primary volume descriptor region at byte offset 32768.
     *
     * @param[in,out] mdfFile Open binary input stream for the MDF image.
     * @return true if the header check passes, false on read failure or matched signature.
     */
    bool validateHeaderOffset(std::ifstream& mdfFile) {
        mdfFile.seekg(32768);

        char buf[12];

        return mdfFile.read(buf, 8) &&
               std::memcmp("CD001", buf + 1, 5) != 0;
    }

    /**
     * @brief Extracts sector data payloads from the MDF stream and writes them to the ISO stream.
     *
     * @param[in,out] mdfFile Source MDF binary input stream.
     * @param[in,out] isoFile Destination ISO binary output stream.
     * @param[in] isoPath Path to the destination ISO file (used on cancellation cleanup).
     * @param[in] layout Resolved sector geometry for reading and skipping bytes.
     * @param[in] sectorCount Total number of sectors to process.
     * @param[in,out] completedBytes Optional atomic counter incremented by payload bytes written.
     * @return true if all sectors are written without error or cancellation, false otherwise.
     */
    bool copySectors(
        std::ifstream& mdfFile,
        std::ofstream& isoFile,
        const std::string& isoPath,
        const MdfTypeInfo& layout,
        size_t sectorCount,
        std::atomic<size_t>* completedBytes) {

        std::vector<char> sectorBuffer(layout.sector_data);

        while (sectorCount > 0) {
            if (consumeCancellationFlag()) {
                return abortConversion(isoFile, isoPath);
            }

            mdfFile.seekg(
                static_cast<std::streamoff>(layout.seek_head),
                std::ios::cur
            );

            if (!mdfFile.read(sectorBuffer.data(), layout.sector_data)) {
                return false;
            }

            mdfFile.seekg(
                static_cast<std::streamoff>(layout.seek_ecc),
                std::ios::cur
            );

            if (consumeCancellationFlag()) {
                return abortConversion(isoFile, isoPath);
            }

            if (!isoFile.write(sectorBuffer.data(), layout.sector_data)) {
                return false;
            }

            if (completedBytes) {
                completedBytes->fetch_add(
                    layout.sector_data,
                    std::memory_order_relaxed
                );
            }

            --sectorCount;
        }

        return true;
    }

} // namespace

/**
 * @brief Converts an MDF disc image into an ISO image by stripping sector headers and ECC data.
 *
 * @param[in] mdfPath Path to the input MDF file.
 * @param[in] isoPath Path to the output ISO file.
 * @param[in,out] completedBytes Optional atomic byte counter updated as payload data is written.
 * @return true if conversion completes successfully, false on I/O failure or cancellation.
 */
bool convertMdfToIso(
    const std::string& mdfPath,
    const std::string& isoPath,
    std::atomic<size_t>* completedBytes) {

    if (consumeCancellationFlag()) {
        return false;
    }

    std::ifstream mdfFile(mdfPath, std::ios::binary);

    if (!mdfFile.is_open() || !validateHeaderOffset(mdfFile)) {
        return false;
    }

    if (consumeCancellationFlag()) {
        return false;
    }

    std::ofstream isoFile(isoPath, std::ios::binary);

    if (!isoFile.is_open()) {
        return false;
    }

    MdfTypeInfo layout;

    if (!layout.determineMdfType(mdfFile)) {
        return false;
    }

    mdfFile.seekg(0, std::ios::end);

    const size_t sourceLength =
        static_cast<size_t>(mdfFile.tellg()) / layout.sector_size;

    mdfFile.seekg(0, std::ios::beg);

    return copySectors(
        mdfFile,
        isoFile,
        isoPath,
        layout,
        sourceLength,
        completedBytes
    );
}
