// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Project Headers
#include "isocmd/converters/ccd.h"
#include "isocmd/core/cancellation.h"

namespace fs = std::filesystem;

// Special thanks to the original authors of the conversion tools:

// Salvatore Santagati (mdf2iso).
// Grégory Kokanosky (nrg2iso).
// Danny Kurniawan and Kerry Harris (ccd2iso).

// Note: Their original code has been modernized and ported to C++.


// MDF2ISO

/*  $Id: mdf2iso.c, 22/05/05

    Copyright (C) 2004,2005 Salvatore Santagati <salvatore.santagati@gmail.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the
    Free Software Foundation, Inc.,
    59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/


namespace {

/**
 * @brief DiscDescriptor sector layout parameters for an MDF source image.
 */
struct SectorLayout {
    size_t seekEcc = 0;     ///< Bytes to skip after sector payload (ECC/EDC/subchannel).
    size_t sectorSize = 0;  ///< Total raw sector stride in bytes.
    size_t seekHead = 0;    ///< Header bytes to skip before sector payload.
    size_t sectorData = 0;  ///< Payload byte count extracted per sector.
};

/**
 * @brief Checks and latches the global operation cancellation flag.
 *
 * @return true if cancellation was requested, false otherwise.
 */
static bool consumeCancellationFlag() {
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
static bool abortConversion(std::ofstream& isoFile, const std::string& isoPath) {
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
static bool validateHeaderOffset(std::ifstream& mdfFile) {
    mdfFile.seekg(32768);
    char buf[12];
    return mdfFile.read(buf, 8) && std::memcmp("CD001", buf + 1, 5) != 0;
}

/**
 * @brief Detects the sector geometry from CD/DVD sync patterns in the MDF stream.
 *
 * @param[in,out] mdfFile Open binary input stream positioned/seekable from offset 0.
 * @param[out] layout Populated with header, payload, ECC, and total sector sizes.
 * @return true if sector probing succeeded, false if initial stream reads failed.
 */
static bool detectSectorLayout(std::ifstream& mdfFile, SectorLayout& layout) {
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
        layout.seekHead = 16;
        layout.sectorData = 2048;
        if (std::memcmp(kSyncPattern, buf, 12) == 0) {
            layout.seekEcc = 288;
            layout.sectorSize = 2352;
        } else {
            layout.seekEcc = 384;
            layout.sectorSize = 2448;
        }
        return true;
    }

    layout.seekHead = 0;
    layout.sectorSize = 2448;
    layout.seekEcc = 96;
    layout.sectorData = 2352;
    return true;
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
static bool copySectors(
    std::ifstream& mdfFile,
    std::ofstream& isoFile,
    const std::string& isoPath,
    const SectorLayout& layout,
    size_t sectorCount,
    std::atomic<size_t>* completedBytes) {
    std::vector<char> sectorBuffer(layout.sectorData);

    while (sectorCount > 0) {
        if (consumeCancellationFlag()) {
            return abortConversion(isoFile, isoPath);
        }

        mdfFile.seekg(static_cast<std::streamoff>(layout.seekHead), std::ios::cur);
        if (!mdfFile.read(sectorBuffer.data(), layout.sectorData)) {
            return false;
        }

        mdfFile.seekg(static_cast<std::streamoff>(layout.seekEcc), std::ios::cur);

        if (consumeCancellationFlag()) {
            return abortConversion(isoFile, isoPath);
        }

        if (!isoFile.write(sectorBuffer.data(), layout.sectorData)) {
            return false;
        }

        if (completedBytes) {
            completedBytes->fetch_add(layout.sectorData, std::memory_order_relaxed);
        }

        --sectorCount;
    }

    return true;
}

}  // namespace

/**
 * @brief Converts an MDF disc image into an ISO image by stripping sector headers and ECC data.
 *
 * @param[in] mdfPath Path to the input MDF file.
 * @param[in] isoPath Path to the output ISO file.
 * @param[in,out] completedBytes Optional atomic byte counter updated as payload data is written.
 * @return true if conversion completes successfully, false on I/O failure or cancellation.
 */
bool convertMdfToIso(const std::string& mdfPath, const std::string& isoPath, std::atomic<size_t>* completedBytes) {
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

    SectorLayout layout;
    if (!detectSectorLayout(mdfFile, layout)) {
        return false;
    }

    mdfFile.seekg(0, std::ios::end);
    const size_t sourceLength = static_cast<size_t>(mdfFile.tellg()) / layout.sectorSize;
    mdfFile.seekg(0, std::ios::beg);

    return copySectors(mdfFile, isoFile, isoPath, layout, sourceLength, completedBytes);
}


// CCD2ISO

/***************************************************************************
 *   Copyright (C) 2003 by Danny Kurniawan                                 *
 *   danny_kurniawan@users.sourceforge.net                                 *
 *                                                                         *
 *   Contributors:                                                         *
 *   - Kerry Harris <tomatoe-source@users.sourceforge.net>                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/


/**
 * @brief Converts a CloneCD (.img/.ccd raw sector stream) disc image into a raw ISO image.
 *
 * Reads sequential `CcdSector` records from `ccdPath`, extracts payload data of size
 * `DATA_SIZE` for Mode 1 and Mode 2 sectors, and writes them to `isoPath`. Encountering
 * sector mode `0xE2` terminates conversion early and returns `true`; any unsupported sector
 * mode or I/O failure returns `false`.
 *
 * The function polls `GlobalState::g_operationCancelled` before and during sector
 * processing. If cancellation is detected after opening the output file, the stream is
 * closed and the partial output file at `isoPath` is removed before returning `false`.
 *
 * @param[in] ccdPath Path to the source binary sector image file.
 * @param[in] isoPath Destination path for the generated ISO output file.
 * @param[out] completedBytes Optional atomic counter incremented by `bytesWritten`
 *             (`DATA_SIZE`) after each sector is written using relaxed memory ordering.
 *             May be `nullptr` if progress tracking is not needed.
 *
 * @return `true` if conversion completes or stops cleanly at sector mode `0xE2`;
 *         `false` if cancelled, if files cannot be opened/written, or if an unexpected
 *         sector mode is encountered.
 *
 * @note On failure or cancellation after output creation, partial files are cleaned up only
 *       on cancellation paths; I/O or unsupported-mode failures return `false` without
 *       explicitly removing `isoPath`.
 * @warning `CcdSector` is read directly into memory via `reinterpret_cast<char*>`; ensure
 *          `CcdSector` is trivially copyable with layout matching the input stream.
 */
bool convertCcdToIso(const std::string& ccdPath, const std::string& isoPath, std::atomic<size_t>* completedBytes) {
    if (GlobalState::g_operationCancelled.load()) {
        GlobalState::g_operationCancelled.store(true);
        return false;
    }

    std::ifstream ccdFile(ccdPath, std::ios::binary);
    if (!ccdFile) return false;
    std::ofstream isoFile(isoPath, std::ios::binary);
    if (!isoFile) return false;

    CcdSector sector;

    while (ccdFile.read(reinterpret_cast<char*>(&sector), sizeof(CcdSector))) {
        if (GlobalState::g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(isoPath);
            GlobalState::g_operationCancelled.store(true);
            return false;
        }
        size_t bytesWritten = 0;

        switch (sector.sectheader.header.mode) {
            case 1: {
                isoFile.write(reinterpret_cast<char*>(sector.content.mode1.data), DATA_SIZE);
                bytesWritten = DATA_SIZE;
                break;
            }
            case 2: {
                isoFile.write(reinterpret_cast<char*>(sector.content.mode2.data), DATA_SIZE);
                bytesWritten = DATA_SIZE;
                break;
            }
            case 0xe2:
                return true;
            default:
                return false;
        }
        if (GlobalState::g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(isoPath);
            GlobalState::g_operationCancelled.store(true);
            return false;
        }
        if (!isoFile || bytesWritten != DATA_SIZE) {
            return false;
        }
        if (completedBytes) {
            completedBytes->fetch_add(bytesWritten, std::memory_order_relaxed);
        }
        if (GlobalState::g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(isoPath);
            GlobalState::g_operationCancelled.store(true);
            return false;
        }

    }
    return true;
}


// NRG2ISO

/*
   29/04/2021 Nrg2Iso v0.4.1

   Copyright (C) 2003-2021 Gregory Kokanosky <gregory.kokanosky@free.fr>

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/


/**
 * @brief Converts a Nero NRG disc image into a raw ISO image by skipping a 307,200-byte header.
 *
 * Opens `inputFile` in binary mode, checks the primary volume descriptor signature
 * (`\x01CD001\x01\x00`) at byte offset `16 * 2048` (`32768`) to reject files already
 * carrying that ISO descriptor header, and seeks to byte offset `307200` (`300 KiB`)
 * before streaming remaining bytes in `1 MiB` chunks to `outputFile`.
 *
 * The function checks `GlobalState::g_operationCancelled` prior to opening streams and
 * during the copy loop. If cancelled after `outputFile` is opened, the output stream is
 * closed and `outputFile` is removed before returning `false`. A failed write also closes
 * `outputFile` and removes the partial output file before returning `false`.
 *
 * @param[in] inputFile Path to the source NRG disc image file.
 * @param[in] outputFile Destination path for the generated ISO output file.
 * @param[out] completedBytes Optional atomic counter incremented by `bytesRead` after each
 *             chunk is written using relaxed memory ordering. May be `nullptr` if progress
 *             reporting is not required.
 *
 * @return `true` if streaming completes cleanly through EOF; `false` if cancelled, if
 *         `inputFile` or `outputFile` cannot be opened, if the descriptor check matches
 *         the `CD001` signature at offset `32768`, or if writing to `outputFile` fails.
 *
 * @note Any trailing NRG footer/chunk metadata after the payload stream is copied as-is
 *       because the loop copies until EOF or stream failure.
 */
bool convertNrgToIso(const std::string& inputFile, const std::string& outputFile, std::atomic<size_t>* completedBytes) {
    if (GlobalState::g_operationCancelled.load()) {
        GlobalState::g_operationCancelled.store(true);
        return false;
    }

    std::ifstream nrgFile(inputFile, std::ios::binary);
    if (!nrgFile) {
        return false;
    }

    nrgFile.seekg(0, std::ios::end);
    nrgFile.seekg(0, std::ios::beg);

    constexpr size_t ISO_CHECK_OFFSET = 16 * 2048;
    char isoBuf[8];
    nrgFile.seekg(ISO_CHECK_OFFSET);
    nrgFile.read(isoBuf, 8);

    if (memcmp(isoBuf, "\x01" "CD001" "\x01\x00", 8) == 0) {
        return false;
    }

    nrgFile.clear();
    nrgFile.seekg(307200, std::ios::beg);

    if (GlobalState::g_operationCancelled.load()) {
        GlobalState::g_operationCancelled.store(true);
        return false;
    }

    std::ofstream isoFile(outputFile, std::ios::binary);
    if (!isoFile) {
        return false;
    }

    constexpr size_t BUFFER_SIZE = 1024 * 1024;
    std::vector<char> buffer(BUFFER_SIZE);

    while (nrgFile) {
        if (GlobalState::g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(outputFile);
            GlobalState::g_operationCancelled.store(true);
            return false;
        }

        nrgFile.read(buffer.data(), BUFFER_SIZE);
        std::streamsize bytesRead = nrgFile.gcount();

        if (bytesRead > 0) {
            if (GlobalState::g_operationCancelled.load()) {
                isoFile.close();
                fs::remove(outputFile);
                GlobalState::g_operationCancelled.store(true);
                return false;
            }

            if (!isoFile.write(buffer.data(), bytesRead)) {
                isoFile.close();
                fs::remove(outputFile);
                return false;
            }

            if (completedBytes) {
                completedBytes->fetch_add(bytesRead, std::memory_order_relaxed);
            }
        }

        if (nrgFile.eof() || nrgFile.fail()) {
            break;
        }
    }

    return true;
}
