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
#include "isocmd/core/cancellation.h"

namespace fs = std::filesystem;

// Special thanks to Grégory Kokanosky (nrg2iso).

/*
   29/04/2021 Nrg2Iso v0.4.1

   Copyright (C) 2003-2021 Gregory Kokanosky <gregory.kokanosky@free.fr>

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program. If not, see <https://www.gnu.org/licenses/>.
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
bool convertNrgToIso(
    const std::string& inputFile,
    const std::string& outputFile,
    std::atomic<size_t>* completedBytes) {

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

    if (std::memcmp(isoBuf, "\x01" "CD001" "\x01\x00", 8) == 0) {
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
        const std::streamsize bytesRead = nrgFile.gcount();

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
                completedBytes->fetch_add(
                    static_cast<size_t>(bytesRead),
                    std::memory_order_relaxed
                );
            }
        }

        if (nrgFile.eof() || nrgFile.fail()) {
            break;
        }
    }

    return true;
}
