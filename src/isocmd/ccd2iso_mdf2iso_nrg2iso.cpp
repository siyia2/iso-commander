// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../mdf.h"
#include "../ccd.h"


// Special thanks to the original authors of the conversion tools:

// Salvatore Santagati (mdf2iso).
// Grégory Kokanosky (nrg2iso).
// Danny Kurniawan and Kerry Harris (ccd2iso).

// Note: Their original code has been modernized and ported to C++.


// MDF2ISO

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
    
    // Disable internal buffering for more direct writes
    isoFile.rdbuf()->pubsetbuf(nullptr, 0);
    
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

    // Disable internal buffering for more direct writes
    isoFile.rdbuf()->pubsetbuf(nullptr, 0);
    
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

    // Check cancellation before opening output file
    if (g_operationCancelled.load()) {
		g_operationCancelled.store(true);
        return false;
    }

    std::ofstream isoFile(outputFile, std::ios::binary);
    if (!isoFile) {
        return false;
    }

    // Disable internal buffering for more direct writes
    isoFile.rdbuf()->pubsetbuf(nullptr, 0);

    // Initialize completedBytes to 0 if provided
    if (completedBytes) {
        *completedBytes = 0;
    }

    constexpr size_t SECTOR_SIZE = 2048;
    std::vector<char> sectorBuffer(SECTOR_SIZE);

    // Conversion loop with comprehensive cancellation checks
    while (nrgFile && nrgFile.tellg() < nrgFileSize) {
        // Check cancellation before reading
        if (g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(outputFile);
            g_operationCancelled.store(true);
            return false;
        }

        nrgFile.read(sectorBuffer.data(), SECTOR_SIZE);
        std::streamsize bytesRead = nrgFile.gcount();
        
        if (bytesRead > 0) {
            // Check cancellation before writing
            if (g_operationCancelled.load()) {
                isoFile.close();
                fs::remove(outputFile);
                g_operationCancelled.store(true);
                return false;
            }

            if (!isoFile.write(sectorBuffer.data(), bytesRead)) {
                return false;
            }

            // Check cancellation after writing
            if (g_operationCancelled.load()) {
                isoFile.close();
                fs::remove(outputFile);
                g_operationCancelled.store(true);
                return false;
            }

            // Update progress
            if (completedBytes) {
                completedBytes->fetch_add(bytesRead, std::memory_order_relaxed);
            }
        }
    }

    return true;
}
