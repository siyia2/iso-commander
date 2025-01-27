// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"
#include "../mdf.h"
#include "../ccd.h"

namespace fs = std::filesystem;

// Special thanks to the original authors of the conversion tools:

// Salvatore Santagati (mdf2iso).
// Grégory Kokanosky (nrg2iso).
// Danny Kurniawan and Kerry Harris (ccd2iso).

// Note: Their original code has been modernized and ported to C++.


// MDF2ISO

bool convertMdfToIso(const std::string& mdfPath, const std::string& isoPath, std::atomic<size_t>* completedBytes) {
    std::ifstream mdfFile(mdfPath, std::ios::binary);
    std::ofstream isoFile(isoPath, std::ios::binary);
    if (!mdfFile.is_open() || !isoFile.is_open()) {
        return false;
    }

    // Disable internal buffering for more direct writes
    isoFile.rdbuf()->pubsetbuf(nullptr, 0);

    size_t seek_ecc = 0, sector_size = 0, seek_head = 0, sector_data = 0;
    char buf[12];
    
    // Check if file is valid MDF
    mdfFile.seekg(32768);
    if (!mdfFile.read(buf, 8) || std::memcmp("CD001", buf + 1, 5) == 0) {
        return false; // Not an MDF file or unsupported format
    }
    
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
    
    // Buffer for a single sector
    std::vector<char> sectorBuffer(sector_data);
    size_t sectorsProcessed = 0;

    while (source_length > 0) {
        if (g_operationCancelled) {
            isoFile.close();
            fs::remove(isoPath);
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
        
        // Write sector immediately
        if (!isoFile.write(sectorBuffer.data(), sector_data)) {
            return false;
        }

        // Update progress
        if (completedBytes) {
            completedBytes->fetch_add(sector_data, std::memory_order_relaxed);
        }
        
        sectorsProcessed++;
        --source_length;
    }

    return true;
}


// CCD2ISO

bool convertCcdToIso(const std::string& ccdPath, const std::string& isoPath, std::atomic<size_t>* completedBytes) {
    std::ifstream ccdFile(ccdPath, std::ios::binary);
    if (!ccdFile) return false;
    
    std::ofstream isoFile(isoPath, std::ios::binary);
    if (!isoFile) return false;

    // Disable internal buffering for more direct writes
    isoFile.rdbuf()->pubsetbuf(nullptr, 0);
    
    CcdSector sector;
    size_t sectorNum = 0;
    
    while (ccdFile.read(reinterpret_cast<char*>(&sector), sizeof(CcdSector))) {
        if (g_operationCancelled) {
            isoFile.close();
            fs::remove(isoPath);
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

        if (!isoFile || bytesWritten != DATA_SIZE) {
            return false;
        }

        if (completedBytes) {
            completedBytes->fetch_add(bytesWritten, std::memory_order_relaxed);
        }
        
        sectorNum++;
    }

    return true;
}

// NRG2ISO

bool convertNrgToIso(const std::string& inputFile, const std::string& outputFile, std::atomic<size_t>* completedBytes) {
    std::ifstream nrgFile(inputFile, std::ios::binary);
    if (!nrgFile) {
        return false;
    }

    // Get the size of the input file
    nrgFile.seekg(0, std::ios::end);
    std::streamsize nrgFileSize = nrgFile.tellg();
    nrgFile.seekg(0, std::ios::beg);

    // Check if the file is already in ISO format
    nrgFile.seekg(16 * 2048);
    char isoBuf[8];
    nrgFile.read(isoBuf, 8);
    
    if (memcmp(isoBuf, "\x01" "CD001" "\x01\x00", 8) == 0) {
        return false;  // Already an ISO, no conversion needed
    }

    // Reopen file for conversion
    nrgFile.clear();
    nrgFile.seekg(307200, std::ios::beg);  // Skip the header section

    std::ofstream isoFile(outputFile, std::ios::binary);
    if (!isoFile) {
        return false;
    }

    // Disable internal buffering for more direct writes
    isoFile.rdbuf()->pubsetbuf(nullptr, 0);

    // Use sector-sized buffer (typical CD sector size is 2048 bytes)
    constexpr size_t SECTOR_SIZE = 2048;
    std::vector<char> sectorBuffer(SECTOR_SIZE);
    size_t sectorsProcessed = 0;

    // Initialize completedBytes to 0 if provided
    if (completedBytes) {
        *completedBytes = 0;
    }

    // Read and write sector by sector
    while (nrgFile && nrgFile.tellg() < nrgFileSize) {
        if (g_operationCancelled) {
            isoFile.close();
            fs::remove(outputFile);
            return false;
        }

        nrgFile.read(sectorBuffer.data(), SECTOR_SIZE);
        std::streamsize bytesRead = nrgFile.gcount();
        
        if (bytesRead > 0) {
            if (!isoFile.write(sectorBuffer.data(), bytesRead)) {
                return false;
            }

            // Update progress
            if (completedBytes) {
                completedBytes->fetch_add(bytesRead, std::memory_order_relaxed);
            }
            
            sectorsProcessed++;
        }
    }

    return true;
}
