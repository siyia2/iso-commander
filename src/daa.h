// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DAA_H
#define DAA_H

// Special thanks to the original author of daa2iso:
// Luigi Auriemma (aluigi@autistici.org, http://aluigi.org)
// Note: The original C code has been modernized and ported to C++.
/***************************************************************************
 *   Original DAA2ISO                                                       *
 *   Copyright (C) 2009 Luigi Auriemma                                     *
 *   aluigi@autistici.org                                                   *
 *   http://aluigi.altervista.org                                           *
 *                                                                          *
 *   This program is free software; you can redistribute it and/or modify   *
 *   it under the terms of the GNU General Public License as published by   *
 *   the Free Software Foundation; either version 2 of the License, or      *
 *   (at your option) any later version.                                    *
 *                                                                          *
 *   This program is distributed in the hope that it will be useful,        *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 *   GNU General Public License for more details.                           *
 ***************************************************************************/

#include <cstdint>
#include <string>

// DAA file signature (16 bytes, null-padded)
static constexpr char DAA_SIGNATURE[]    = "DAA VOL\0";   // split-part volumes
static constexpr char DAA_SIGNATURE_MAIN[] = "DAA ";      // single / first file

// Compression types stored per-chunk
enum DaaChunkType : uint8_t {
    DAA_CHUNK_UNCOMPRESSED = 0,
    DAA_CHUNK_ZLIB         = 1,
    DAA_CHUNK_LZMA         = 2,
};

// Main DAA file header (little-endian, packed)
struct __attribute__((packed)) DaaHeader {
    char     signature[16];     // "DAA VOL\0\0\0\0\0\0\0\0\0" or first-file sig
    uint32_t data_offset;       // offset of chunk data area inside file
    uint32_t b;                 // unused / reserved
    uint32_t chunk_size;        // uncompressed size of each chunk (e.g. 0x8000)
    uint64_t iso_size;          // total uncompressed ISO size (bytes)
    uint32_t version;           // format version
    uint32_t c;                 // unused
    uint32_t d;                 // unused
    uint32_t crc;               // CRC32 of the first 0x50 bytes (with crc field = 0)
};

// Each entry in the chunk-offset table is 4 bytes in the original format.
// The table has (num_chunks + 1) entries so consecutive subtraction gives
// compressed sizes. The value is relative to the start of the data area.
// (In some extended versions the table entry can also encode the chunk type
//  in the high bits – we handle that below.)

#endif // DAA_H
