// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../mdf.h"
#include "../ccd.h"
#include "../daa.h"
#include <zlib.h>
#include <lzma.h>


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


bool convertMdfToIso(const std::string& mdfPath, const std::string& isoPath, std::atomic<size_t>* completedBytes) {
    // Early cancellation check
    if (g_operationCancelled.load()) {
        g_operationCancelled.store(true);
        return false;
    }
        
    std::ifstream mdfFile(mdfPath, std::ios::binary);
    if (!mdfFile.is_open()) {
        return false;
    }
    
    // Check if file is valid MDF
    mdfFile.seekg(32768);
    char buf[12];
    if (!mdfFile.read(buf, 8) || std::memcmp("CD001", buf + 1, 5) == 0) {
        return false; // Not an MDF file or unsupported format
    }
    
    // Check cancellation before opening output file
    if (g_operationCancelled.load()) {
        g_operationCancelled.store(true);
        return false;
    }
    
    std::ofstream isoFile(isoPath, std::ios::binary);
    if (!isoFile.is_open()) {
        return false;
    }
    
    // Enable internal buffering (by default standard buffer size is utilized)
    // Optionally, you can set a custom buffer size like this:
    // char buffer[65536]; // 64KB buffer
    // isoFile.rdbuf()->pubsetbuf(buffer, sizeof(buffer));
    
    // Determine MDF format
    size_t seek_ecc = 0, sector_size = 0, seek_head = 0, sector_data = 0;
    
    mdfFile.seekg(0);
    if (!mdfFile.read(buf, 12)) {
        return false;
    }
    
    // Determine MDF type based on sync patterns
    if (std::memcmp("\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00", buf, 12) == 0) {
        mdfFile.seekg(2352);
        if (!mdfFile.read(buf, 12)) {
            return false;
        }
        if (std::memcmp("\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00", buf, 12) == 0) {
            seek_ecc = 288;
            sector_size = 2352;
            sector_data = 2048;
            seek_head = 16;
        } else {
            seek_ecc = 384;
            sector_size = 2448;
            sector_data = 2048;
            seek_head = 16;
        }
    } else {
        seek_head = 0;
        sector_size = 2448;
        seek_ecc = 96;
        sector_data = 2352;
    }
    
    // Calculate the number of sectors
    mdfFile.seekg(0, std::ios::end);
    size_t source_length = static_cast<size_t>(mdfFile.tellg()) / sector_size;
    mdfFile.seekg(0, std::ios::beg);
    
    // Initialize progress tracking
    if (completedBytes) {
        *completedBytes = 0;
    }
    
    // Buffer for a single sector
    std::vector<char> sectorBuffer(sector_data);
    
    // Main conversion loop with strategic cancellation checks
    while (source_length > 0) {
        // Check cancellation at the start of each sector processing
        if (g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(isoPath);
            g_operationCancelled.store(true);
            return false;
        }
        
        // Skip header
        mdfFile.seekg(static_cast<std::streamoff>(seek_head), std::ios::cur);
        
        // Read sector data
        if (!mdfFile.read(sectorBuffer.data(), sector_data)) {
            return false;
        }
        
        // Skip ECC data
        mdfFile.seekg(static_cast<std::streamoff>(seek_ecc), std::ios::cur);
        
        // Check cancellation before writing
        if (g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(isoPath);
            g_operationCancelled.store(true);
            return false;
        }
        
        // Write sector
        if (!isoFile.write(sectorBuffer.data(), sector_data)) {
            return false;
        }
        
        // Update progress
        if (completedBytes) {
            completedBytes->fetch_add(sector_data, std::memory_order_relaxed);
        }
        
        --source_length;
    }
    
    return true;
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
 

bool convertCcdToIso(const std::string& ccdPath, const std::string& isoPath, std::atomic<size_t>* completedBytes) {
   // Early cancellation check
    if (g_operationCancelled.load()) {
        g_operationCancelled.store(true);
        return false;
    }
        
    std::ifstream ccdFile(ccdPath, std::ios::binary);
    if (!ccdFile) return false;
    std::ofstream isoFile(isoPath, std::ios::binary);
    if (!isoFile) return false;
    
    // Enable internal buffering (by default standard buffer size is utilized)
    // Optionally, you can set a custom buffer size like this:
    // char buffer[65536]; // 64KB buffer
    // isoFile.rdbuf()->pubsetbuf(buffer, sizeof(buffer));
    
    CcdSector sector;
    size_t sectorNum = 0;
    
    while (ccdFile.read(reinterpret_cast<char*>(&sector), sizeof(CcdSector))) {
        // Check cancellation at the start of the loop
        if (g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(isoPath);
            g_operationCancelled.store(true);
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
                // Found session marker
                return true;
            default:
                return false;
        }
        // Check cancellation immediately after writing sector data
        if (g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(isoPath);
            g_operationCancelled.store(true);
            return false;
        }
        // Validate write operation
        if (!isoFile || bytesWritten != DATA_SIZE) {
            return false;
        }
        // Update progress
        if (completedBytes) {
            completedBytes->fetch_add(bytesWritten, std::memory_order_relaxed);
        }
        // Check cancellation after updating progress
        if (g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(isoPath);
            g_operationCancelled.store(true);
            return false;
        }
        
        sectorNum++;
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


bool convertNrgToIso(const std::string& inputFile, const std::string& outputFile, std::atomic<size_t>* completedBytes) {
    // Early cancellation check
    if (g_operationCancelled.load()) {
        g_operationCancelled.store(true);
        return false;
    }
    
    std::ifstream nrgFile(inputFile, std::ios::binary);
    if (!nrgFile) {
        return false;
    }
    
    // Get the size of the input file
    nrgFile.seekg(0, std::ios::end);
    nrgFile.seekg(0, std::ios::beg);
    
    // Check if the file is already in ISO format (using the same logic as the C version)
    constexpr size_t ISO_CHECK_OFFSET = 16 * 2048;
    char isoBuf[8];
    nrgFile.seekg(ISO_CHECK_OFFSET);
    nrgFile.read(isoBuf, 8);
    
    if (memcmp(isoBuf, "\x01" "CD001" "\x01\x00", 8) == 0) {
        return false;  // Already an ISO, no conversion needed
    }
    
    // Reopen file for conversion
    nrgFile.clear();
    nrgFile.seekg(307200, std::ios::beg);  // Skip the header section
    
    // Check cancellation before opening output file
    if (g_operationCancelled.load()) {
        g_operationCancelled.store(true);
        return false;
    }
    
    std::ofstream isoFile(outputFile, std::ios::binary);
    if (!isoFile) {
        return false;
    }
    
    // Initialize completedBytes to 0 if provided
    if (completedBytes) {
        *completedBytes = 0;
    }
    
    // Use the 1MB buffer size from the C version
    constexpr size_t BUFFER_SIZE = 1024 * 1024;
    std::vector<char> buffer(BUFFER_SIZE);
    
    // Conversion loop with comprehensive cancellation checks
    while (nrgFile) {
        // Check cancellation before reading
        if (g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(outputFile);
            g_operationCancelled.store(true);
            return false;
        }
        
        nrgFile.read(buffer.data(), BUFFER_SIZE);
        std::streamsize bytesRead = nrgFile.gcount();
        
        if (bytesRead > 0) {
            // Check cancellation before writing
            if (g_operationCancelled.load()) {
                isoFile.close();
                fs::remove(outputFile);
                g_operationCancelled.store(true);
                return false;
            }
            
            if (!isoFile.write(buffer.data(), bytesRead)) {
                isoFile.close();
                fs::remove(outputFile);
                return false;
            }
            
            // Update progress
            if (completedBytes) {
                completedBytes->fetch_add(bytesRead, std::memory_order_relaxed);
            }
        }
        
        // Break if we've reached EOF or an error occurred
        if (nrgFile.eof() || nrgFile.fail()) {
            break;
        }
    }
    
    return true;
}


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
 
 
// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

 
// Decompress one zlib-deflate chunk.
// Returns true on success and fills `out` (exactly chunk_size bytes).
static bool daa_inflate_chunk(const uint8_t* src, size_t src_len,
                               uint8_t* dst, uint32_t dst_len) {
    uLongf out_len = dst_len;
    int ret = uncompress(dst, &out_len, src, static_cast<uLong>(src_len));
    if (ret != Z_OK) {
        // Some DAA files use raw deflate without the zlib wrapper.
        // Fall back to inflateRaw.
        z_stream strm{};
        strm.next_in  = const_cast<Bytef*>(src);
        strm.avail_in = static_cast<uInt>(src_len);
        strm.next_out = dst;
        strm.avail_out = dst_len;
        if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) return false;
        ret = inflate(&strm, Z_FINISH);
        inflateEnd(&strm);
        if (ret != Z_STREAM_END) return false;
        out_len = strm.total_out;
    }
    return (out_len == dst_len);
}
 
// Decompress one LZMA chunk.
static bool daa_lzma_chunk(const uint8_t* src, size_t src_len,
                             uint8_t* dst, uint32_t dst_len) {
    // LZMA chunks in DAA start with a 5-byte LZMA properties header followed
    // by raw compressed data (no stream-end marker).
    constexpr size_t LZMA_LEGACY_PROPS = 5; // lc/lp/pb byte + 4-byte dict_size
    if (src_len < LZMA_LEGACY_PROPS) return false;
 
    lzma_filter filters[2];
    lzma_options_lzma lzma_opts;
    if (lzma_lzma_preset(&lzma_opts, LZMA_PRESET_DEFAULT)) return false;
 
    // Parse the embedded LZMA properties (lc, lp, pb, dict_size)
    const uint8_t props = src[0];
    if (props >= (9 * 5 * 5)) return false;
    lzma_opts.pb = props / (9 * 5);
    lzma_opts.lp = (props % (9 * 5)) / 9;
    lzma_opts.lc = props % 9;
    lzma_opts.dict_size = 0;
    for (int i = 0; i < 4; i++)
        lzma_opts.dict_size |= static_cast<uint32_t>(src[1 + i]) << (8 * i);
 
    filters[0] = { LZMA_FILTER_LZMA1, &lzma_opts };
    filters[1] = { LZMA_VLI_UNKNOWN,  nullptr    };
 
    size_t in_pos  = LZMA_LEGACY_PROPS;
    size_t out_pos = 0;
    lzma_ret lret = lzma_raw_buffer_decode(
        filters, nullptr,
        src, &in_pos, src_len,
        dst, &out_pos, dst_len);
 
    return (lret == LZMA_OK || lret == LZMA_STREAM_END) &&
           (out_pos == static_cast<size_t>(dst_len));
}
 
// ---------------------------------------------------------------------------
// Multi-part DAA helper: builds the ordered list of DAA part file paths.
//
// Naming conventions supported:
//   foo.part01.daa / foo.part001.daa  – split volumes
//   foo.daa                           – single file
// ---------------------------------------------------------------------------
static std::vector<std::string> daa_build_parts(const std::string& first_path) {
    std::vector<std::string> parts;
    parts.push_back(first_path);
 
    // Detect split pattern
    std::string base = first_path;
    // Strip .daa
    if (base.size() > 4 &&
        base.substr(base.size() - 4) == ".daa") {
        base = base.substr(0, base.size() - 4);
    }
 
    // Check for .part01 / .part001 suffixes
    std::string stem     = base;
    int         digits   = 0;
    bool        is_split = false;
 
    // Pattern: ends with .part01 (2-digit) or .part001 (3-digit)?
    auto try_pattern = [&](const std::string& s, int d) -> bool {
        if (s.size() < static_cast<size_t>(d + 5)) return false;
        std::string tail = s.substr(s.size() - d - 5);
        if (tail.substr(0, 5) != ".part") return false;
        for (int i = 5; i < 5 + d; i++)
            if (!std::isdigit(static_cast<unsigned char>(tail[i]))) return false;
        return true;
    };
 
    if (try_pattern(base, 3)) { digits = 3; is_split = true; }
    else if (try_pattern(base, 2)) { digits = 2; is_split = true; }
 
    if (!is_split) return parts; // single file – nothing more to collect
 
    // Trim the existing part suffix to get the real stem
    stem = base.substr(0, base.size() - 5 - digits); // remove ".partNN[N]"
 
    // Collect subsequent parts
    char buf[32];
    for (int n = 2; ; n++) {
        if (digits == 3)
            std::snprintf(buf, sizeof(buf), ".part%03d.daa", n);
        else
            std::snprintf(buf, sizeof(buf), ".part%02d.daa", n);
 
        std::string next = stem + buf;
        if (!fs::exists(next)) break;
        parts.push_back(next);
    }
    return parts;
}
 
// ---------------------------------------------------------------------------
// Core DAA reader state
// ---------------------------------------------------------------------------
struct DaaReader {
    // Opened file handles (one per part)
    std::vector<std::ifstream> files;
    std::vector<uint64_t>      file_iso_end; // cumulative ISO end offset per part
 
    // Header fields we need during conversion
    uint32_t chunk_size   = 0;   // uncompressed bytes per chunk
    uint64_t iso_size     = 0;   // total uncompressed ISO size
    uint32_t num_chunks   = 0;
 
    // Chunk offset table (size = num_chunks + 1), offsets relative to data_offset
    std::vector<uint32_t> chunk_table;
    // Compression type per chunk (extracted from high bits in some DAA versions)
    std::vector<uint8_t>  chunk_type;
 
    uint32_t data_offset = 0;    // offset within the FIRST file where chunk data begins
 
    bool open(const std::vector<std::string>& parts);
};
 
bool DaaReader::open(const std::vector<std::string>& part_paths) {
    if (part_paths.empty()) return false;
 
    // -----------------------------------------------------------------------
    // Open and parse the FIRST file header
    // -----------------------------------------------------------------------
    std::ifstream f0(part_paths[0], std::ios::binary);
    if (!f0) return false;
 
    DaaHeader hdr{};
    f0.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!f0) return false;
 
    // Validate signature – single-file DAA starts with "DAA " or "DAA VOL"
    // (Luigi's code accepts anything with "DAA" in the first 4 bytes as a
    //  fallback, but we are strict for safety.)
    if (std::memcmp(hdr.signature, "DAA VOL", 7) != 0 &&
        std::memcmp(hdr.signature, "DAA ",    4) != 0) {
        return false;
    }
 
    chunk_size  = hdr.chunk_size;
    iso_size    = hdr.iso_size;
    data_offset = hdr.data_offset;
 
    if (chunk_size == 0 || iso_size == 0) return false;
 
    num_chunks = static_cast<uint32_t>(
        (iso_size + chunk_size - 1) / chunk_size);
 
    // -----------------------------------------------------------------------
    // Read the chunk-offset table from the first file.
    // Table has num_chunks+1 entries (32-bit each).  The table starts right
    // after the fixed header (offset sizeof(DaaHeader)).
    // -----------------------------------------------------------------------
    chunk_table.resize(num_chunks + 1);
    chunk_type.resize(num_chunks, DAA_CHUNK_UNCOMPRESSED);
 
    f0.seekg(sizeof(DaaHeader), std::ios::beg);
    f0.read(reinterpret_cast<char*>(chunk_table.data()),
            static_cast<std::streamsize>((num_chunks + 1) * sizeof(uint32_t)));
    if (!f0) return false;
 
    // High 2 bits of each table entry encode the compression type
    // (this is the extended format Luigi added in later versions).
    for (uint32_t i = 0; i < num_chunks; i++) {
        uint8_t  ct = static_cast<uint8_t>(chunk_table[i] >> 30);
        chunk_table[i] &= 0x3FFFFFFFu;
 
        switch (ct) {
            case 0:  chunk_type[i] = DAA_CHUNK_UNCOMPRESSED; break;
            case 1:  chunk_type[i] = DAA_CHUNK_ZLIB;         break;
            case 2:  chunk_type[i] = DAA_CHUNK_LZMA;         break;
            default: chunk_type[i] = DAA_CHUNK_ZLIB;         break; // treat unknown as zlib
        }
    }
    // The last table entry also has its high bits stripped
    chunk_table[num_chunks] &= 0x3FFFFFFFu;
 
    // -----------------------------------------------------------------------
    // Open all part files and calculate their cumulative ISO end offsets.
    // The first file's data runs from data_offset to EOF of that file.
    // -----------------------------------------------------------------------
    files.emplace_back(std::move(f0));
    for (size_t i = 1; i < part_paths.size(); i++) {
        std::ifstream fi(part_paths[i], std::ios::binary);
        if (!fi) return false;
        files.emplace_back(std::move(fi));
    }
 
    // We don't need per-file ISO ranges for the conversion because we address
    // chunks directly through the offset table – all offsets are relative to
    // data_offset in the first file.  Multi-part data is simply concatenated
    // after the first file's EOF, with each subsequent part having a small
    // 16-byte "DAA VOL" header we must skip.
 
    return true;
}
 
// ---------------------------------------------------------------------------
// Read compressed chunk data (spanning parts if needed).
// `offset` is the raw byte offset from the start of the FIRST file's data
// area (i.e. absolute file offset = data_offset + offset).
// ---------------------------------------------------------------------------
static bool daa_read_chunk_data(DaaReader& rdr,
                                 uint64_t   offset,   // offset from data_offset
                                 uint8_t*   buf,
                                 size_t     len) {
    if (len == 0) return true;
 
    // The chunk data across multiple parts: part 0 stores data starting at
    // `data_offset` from its file beginning.  Part N (N>0) starts at byte 16
    // (the 16-byte part-header / signature).
 
    // Build a virtual linear address space:
    //   virtual_offset 0 = byte `data_offset` in part 0
    //   virtual_offset X = wherever that falls in the multi-part sequence.
 
    // Compute sizes of each part's data contribution
    struct PartSpan {
        std::ifstream* file;
        uint64_t data_start; // absolute file offset where data begins in this part
        uint64_t data_size;  // bytes of chunk-data in this part
    };
 
    std::vector<PartSpan> spans;
    for (size_t i = 0; i < rdr.files.size(); i++) {
        auto& f = rdr.files[i];
        f.seekg(0, std::ios::end);
        uint64_t file_size = static_cast<uint64_t>(f.tellg());
        uint64_t dstart = (i == 0) ? rdr.data_offset : 16; // 16-byte vol header
        if (file_size <= dstart) continue;
        spans.push_back({ &f, dstart, file_size - dstart });
    }
 
    // Find which part `offset` falls in
    uint64_t remaining = offset;
    for (auto& sp : spans) {
        if (remaining < sp.data_size) {
            // Start reading here
            uint64_t pos = sp.data_start + remaining;
            size_t   avail = static_cast<size_t>(sp.data_size - remaining);
            size_t   to_read = std::min(avail, len);
            sp.file->seekg(static_cast<std::streamoff>(pos), std::ios::beg);
            sp.file->read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(to_read));
            if (!*sp.file && to_read > 0) return false;
            buf += to_read;
            len -= to_read;
            remaining = 0;
            if (len == 0) return true;
            continue;
        }
        remaining -= sp.data_size;
    }
    return (len == 0);
}
 
// ---------------------------------------------------------------------------
// Main public function – mirrors convertCcdToIso() style
// ---------------------------------------------------------------------------
bool convertDaaToIso(const std::string& daaPath,
                     const std::string& isoPath,
                     std::atomic<size_t>* completedBytes) {
    // Early cancellation check
    if (g_operationCancelled.load()) return false;
 
    // -----------------------------------------------------------------------
    // Collect multi-part files and open the reader
    // -----------------------------------------------------------------------
    std::vector<std::string> parts = daa_build_parts(daaPath);
 
    DaaReader rdr;
    if (!rdr.open(parts)) return false;
 
    // -----------------------------------------------------------------------
    // Open output ISO
    // -----------------------------------------------------------------------
    std::ofstream isoFile(isoPath, std::ios::binary);
    if (!isoFile) return false;
 
    // -----------------------------------------------------------------------
    // Allocate working buffers (worst case: chunk_size for both compressed and
    // uncompressed data; compressed chunk can theoretically be larger than
    // uncompressed for tiny/incompressible chunks, so add headroom).
    // -----------------------------------------------------------------------
    const uint32_t chunk_size = rdr.chunk_size;
    std::vector<uint8_t> comp_buf(chunk_size + chunk_size / 2 + 512);
    std::vector<uint8_t> decomp_buf(chunk_size);
 
    // -----------------------------------------------------------------------
    // Convert chunk by chunk
    // -----------------------------------------------------------------------
    for (uint32_t i = 0; i < rdr.num_chunks; i++) {
        if (g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(isoPath);
            return false;
        }
 
        uint32_t comp_offset = rdr.chunk_table[i];
        uint32_t comp_size   = rdr.chunk_table[i + 1] - rdr.chunk_table[i];
 
        // The last chunk may be smaller than chunk_size
        uint32_t this_chunk_size = chunk_size;
        uint64_t written_so_far  = static_cast<uint64_t>(i) * chunk_size;
        if (written_so_far + chunk_size > rdr.iso_size) {
            this_chunk_size = static_cast<uint32_t>(rdr.iso_size - written_so_far);
        }
 
        const uint8_t* write_ptr = nullptr;
        size_t         write_len = 0;
 
        if (comp_size == 0 || comp_size == this_chunk_size) {
            // Uncompressed: read directly into decomp_buf
            if (!daa_read_chunk_data(rdr, comp_offset,
                                     decomp_buf.data(), this_chunk_size)) {
                isoFile.close();
                fs::remove(isoPath);
                return false;
            }
            write_ptr = decomp_buf.data();
            write_len = this_chunk_size;
        } else {
            // Compressed: read then decompress
            if (comp_size > comp_buf.size()) {
                comp_buf.resize(comp_size);
            }
            if (!daa_read_chunk_data(rdr, comp_offset,
                                     comp_buf.data(), comp_size)) {
                isoFile.close();
                fs::remove(isoPath);
                return false;
            }
 
            uint8_t ct = rdr.chunk_type[i];
            bool ok = false;
            if (ct == DAA_CHUNK_LZMA) {
                ok = daa_lzma_chunk(comp_buf.data(), comp_size,
                                    decomp_buf.data(), this_chunk_size);
            } else {
                // Default: zlib (includes uncompressed-but-different-size edge cases)
                ok = daa_inflate_chunk(comp_buf.data(), comp_size,
                                       decomp_buf.data(), this_chunk_size);
            }
            if (!ok) {
                isoFile.close();
                fs::remove(isoPath);
                return false;
            }
            write_ptr = decomp_buf.data();
            write_len = this_chunk_size;
        }
 
        // Write decompressed data to ISO
        isoFile.write(reinterpret_cast<const char*>(write_ptr),
                      static_cast<std::streamsize>(write_len));
        if (!isoFile) {
            isoFile.close();
            fs::remove(isoPath);
            return false;
        }
 
        if (g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(isoPath);
            return false;
        }
 
        if (completedBytes) {
            completedBytes->fetch_add(write_len, std::memory_order_relaxed);
        }
    }
 
    return true;
}
