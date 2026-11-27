// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <stdint.h>

export module isocmd.converters.daa2iso;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

/*
    Copyright 2007,2008,2009 Luigi Auriemma

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA

    http://www.gnu.org/licenses/gpl-2.0.txt
*/

/// DAA file header structure (packed to 4 bytes)
#pragma pack(4)
struct daa_t {
	uint8_t  sign[16];    /**< File signature ("DAA" or "GBI")          */
	uint32_t size_offset; /**< Offset to size information                */
	uint32_t version;     /**< DAA format version                        */
	uint32_t data_offset; /**< Offset to compressed data                 */
	uint32_t b1;          /**< Compression parameter 1                   */
	uint32_t b0;          /**< Compression parameter 0                   */
	uint32_t chunksize;   /**< Size of compression chunks                */
	uint64_t isosize;     /**< Original uncompressed ISO size            */
	uint64_t daasize;     /**< Compressed DAA file size                  */
	uint8_t  hdata[16];   /**< Header data (reserved)                    */
	uint32_t crc;         /**< CRC32 checksum of the header              */
};
#pragma pack()

/// Byte-swap all multi-byte fields of a daa_t header
void swap_daa_if_be(daa_t *d, int endian);

} // extern "C++"
