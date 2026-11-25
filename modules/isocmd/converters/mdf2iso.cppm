// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <stddef.h>

export module isocmd.converters.mdf2iso;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- converters/mdf2iso ----
// Special thanks to the original author of mdf2iso:

// Salvatore Santagati (mdf2iso).

// Note: The original code has been modernized and ported to C++.

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

/**
 * @struct MdfTypeInfo
 * @brief Stores the detected sector layout parameters of an MDF disc image.
 *
 * Contains the byte offsets and sizes required to extract user data from
 * raw MDF sectors, including sector headers and trailing ECC/subchannel data.
 */
struct MdfTypeInfo {
	size_t seek_ecc;    ///< Number of bytes to skip after the sector payload.
	size_t sector_size; ///< Total size of each raw sector in bytes.
	size_t sector_data; ///< Number of user-data bytes extracted per sector.
	size_t seek_head;   ///< Number of header bytes to skip before the payload.

	/**
	 * @brief Initializes all sector layout parameters to zero.
	 */
	MdfTypeInfo()
	    : seek_ecc(0),
	      sector_size(0),
	      sector_data(0),
	      seek_head(0) {}

	/**
	 * @brief Detects the MDF sector layout by examining CD-ROM sync patterns.
	 */
	bool determineMdfType(std::ifstream& mdfFile);
};

} // extern "C++"
