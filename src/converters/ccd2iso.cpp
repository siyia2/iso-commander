// SPDX-License-Identifier: GPL-3.0-or-later

import std;

import isocmd.converters.ccd2iso;
import isocmd.core.cancellation;

namespace fs = std::filesystem;

// Special thanks to Danny Kurniawan and Kerry Harris (ccd2iso).

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
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the         *
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
bool convertCcdToIso(
    const std::string& ccdPath,
    const std::string& isoPath,
    std::atomic<std::size_t>* completedBytes) {

	if (GlobalState::g_operationCancelled.load()) {
		GlobalState::g_operationCancelled.store(true);
		return false;
	}

	std::ifstream ccdFile(ccdPath, std::ios::binary);
	if (!ccdFile) {
		return false;
	}

	std::ofstream isoFile(isoPath, std::ios::binary);
	if (!isoFile) {
		return false;
	}

	CcdSector sector;

	while (ccdFile.read(reinterpret_cast<char*>(&sector), sizeof(CcdSector))) {
		if (GlobalState::g_operationCancelled.load()) {
			isoFile.close();
			fs::remove(isoPath);
			GlobalState::g_operationCancelled.store(true);
			return false;
		}

		std::size_t bytesWritten = 0;

		switch (sector.sectheader.header.mode) {
			case 1: {
				isoFile.write(
				    reinterpret_cast<char*>(sector.content.mode1.data),
				    DATA_SIZE
				);
				bytesWritten = DATA_SIZE;
				break;
			}

			case 2: {
				isoFile.write(
				    reinterpret_cast<char*>(sector.content.mode2.data),
				    DATA_SIZE
				);
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
			completedBytes->fetch_add(
			    bytesWritten,
			    std::memory_order_relaxed
			);
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
