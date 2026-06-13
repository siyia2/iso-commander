// SPDX-License-Identifier: GPL-3.0-or-later

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

#ifndef DAA2ISO_H
#define DAA2ISO_H

// C++ Standard Library Headers
#include <cstddef>
#include <cstdint>

// ── Type aliases ───────────────────────────────────────────────────────────
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

// ── DAA image type constants ───────────────────────────────────────────────
enum { TYPE_DAA, TYPE_GBI, TYPE_NONE };

/**
 * @brief DAA file header structure (packed to 4 bytes)
 */
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

/**
 * @brief DAA per-chunk index entry (packed to 1 byte, no padding)
 */
#pragma pack(1)
struct daa_data_t {
    uint8_t n1, n2, n3;
};
#pragma pack()

/**
 * @brief Swap byte order of a 32-bit integer if big-endian
 */
inline void swap32_if_be(uint32_t *n, int endian) {
    if (!endian) return;
    uint32_t t = *n;
    *n = ((t & 0xff000000u) >> 24) | ((t & 0x00ff0000u) >> 8)
       | ((t & 0x0000ff00u) <<  8) | ((t & 0x000000ffu) << 24);
}

/**
 * @brief Swap byte order of a 64-bit integer if big-endian
 */
inline void swap64_if_be(uint64_t *n, int endian) {
    if (!endian) return;
    uint64_t t = *n;
    *n = ((t & 0xff00000000000000ULL) >> 56)
       | ((t & 0x00ff000000000000ULL) >> 40)
       | ((t & 0x0000ff0000000000ULL) >> 24)
       | ((t & 0x000000ff00000000ULL) >>  8)
       | ((t & 0x00000000ff000000ULL) <<  8)
       | ((t & 0x0000000000ff0000ULL) << 24)
       | ((t & 0x000000000000ff00ULL) << 40)
       | ((t & 0x00000000000000ffULL) << 56);
}

/**
 * @brief Byte-swap all multi-byte fields of a daa_t header
 */
inline void swap_daa_if_be(daa_t *d, int endian) {
    if (!endian) return;
    swap32_if_be(&d->size_offset, 1);
    swap32_if_be(&d->version,     1);
    swap32_if_be(&d->data_offset, 1);
    swap32_if_be(&d->b1,          1);
    swap32_if_be(&d->b0,          1);
    swap32_if_be(&d->chunksize,   1);
    swap64_if_be(&d->isosize,     1);
    swap64_if_be(&d->daasize,     1);
    swap32_if_be(&d->crc,         1);
}

#endif // DAA2ISO_H
