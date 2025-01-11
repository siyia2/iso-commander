// SPDX-License-Identifier: GNU General Public License v3.0 or later

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
    std::ifstream mdfFile(mdfPath, std::ios::binary);
    std::ofstream isoFile(isoPath, std::ios::binary);

    if (!mdfFile.is_open() || !isoFile.is_open()) {
        return false;
    }

    MdfTypeInfo mdfInfo;
    if (!mdfInfo.determineMdfType(mdfFile)) {
        return false; // Not an MDF file or unsupported format
    }

    // Calculate the number of sectors
    mdfFile.seekg(0, std::ios::end);
    size_t source_length = static_cast<size_t>(mdfFile.tellg()) / mdfInfo.sector_size;
    mdfFile.seekg(0, std::ios::beg);

    // Pre-allocate the ISO file to optimize file write performance
    isoFile.seekp(source_length * mdfInfo.sector_data);
    isoFile.put(0); // Write a dummy byte to allocate space
    isoFile.seekp(0); // Reset the pointer to the beginning of the file

    // Buffer for reading and writing
    const size_t bufferSize = 8 * 1024 * 1024; // 8 MB
    std::vector<char> buffer(bufferSize);
    size_t bufferIndex = 0;

    while (source_length > 0) {
        // Read sector data
        mdfFile.seekg(static_cast<std::streamoff>(mdfInfo.seek_head), std::ios::cur);
        if (!mdfFile.read(buffer.data() + bufferIndex, mdfInfo.sector_data)) {
            return false;
        }
        mdfFile.seekg(static_cast<std::streamoff>(mdfInfo.seek_ecc), std::ios::cur);

        bufferIndex += mdfInfo.sector_data;

        // Write full buffer if filled
        if (bufferIndex >= bufferSize) {
            if (!isoFile.write(buffer.data(), bufferIndex)) {
                return false;
            }
            completedBytes->fetch_add(bufferIndex, std::memory_order_relaxed);
            bufferIndex = 0;
        }

        --source_length;
    }

    // Write any remaining data in the buffer
    if (bufferIndex > 0) {
        if (!isoFile.write(buffer.data(), bufferIndex)) {
            return false;
        }
        if (completedBytes) {
			completedBytes->fetch_add(bufferIndex, std::memory_order_relaxed);
		}
    }

    return true;
}


// CCD2ISO

bool convertCcdToIso(const std::string& ccdPath, const std::string& isoPath, std::atomic<size_t>* completedBytes) {
    std::ifstream ccdFile(ccdPath, std::ios::binary | std::ios::ate);
    if (!ccdFile) return false;

    std::ofstream isoFile(isoPath, std::ios::binary);
    if (!isoFile) return false;

    size_t fileSize = ccdFile.tellg();
    ccdFile.seekg(0, std::ios::beg);

    // Pre-allocate output file to reduce fragmentation
    isoFile.seekp(fileSize / sizeof(CcdSector) * DATA_SIZE); // Preallocate based on expected data size
    isoFile.put(0);  // Write a dummy byte to allocate space
    isoFile.seekp(0); // Reset the pointer to the beginning

    std::vector<uint8_t> buffer(BUFFER_SIZE);
    size_t bufferPos = 0;
    CcdSector sector;
    int sessionCount = 0;

    while (ccdFile.read(reinterpret_cast<char*>(&sector), sizeof(CcdSector))) {
        switch (sector.sectheader.header.mode) {
            case 1:
            case 2: {
                const uint8_t* sectorData = (sector.sectheader.header.mode == 1) 
                    ? sector.content.mode1.data 
                    : sector.content.mode2.data;

                if (bufferPos + DATA_SIZE > BUFFER_SIZE) {
                    isoFile.write(reinterpret_cast<char*>(buffer.data()), bufferPos);
                    if (!isoFile) {
                        return false;
                    }
                    completedBytes->fetch_add(bufferPos, std::memory_order_relaxed);
                    bufferPos = 0;
                }

                // Use faster memcpy
                std::memcpy(&buffer[bufferPos], sectorData, DATA_SIZE);
                bufferPos += DATA_SIZE;
                break;
            }
            case 0xe2:
                sessionCount++;
                break;
            default:
                return false;
        }
    }

    // Flush remaining data
    if (bufferPos > 0) {
        isoFile.write(reinterpret_cast<char*>(buffer.data()), bufferPos);
        if (!isoFile) {
            return false;
        }
         if (completedBytes) {
			completedBytes->fetch_add(bufferPos, std::memory_order_relaxed);
		 }
    }

    return true;
}


// NRG2ISO

bool convertNrgToIso(const std::string& inputFile, const std::string& outputFile, std::atomic<size_t>* completedBytes) {
    std::ifstream nrgFile(inputFile, std::ios::binary | std::ios::ate);  // Open for reading, with positioning at the end
    if (!nrgFile) {
        return false;
    }

    std::streamsize nrgFileSize = nrgFile.tellg();  // Get the size of the input file
    nrgFile.seekg(0, std::ios::beg);  // Rewind to the beginning of the file

    // Check if the file is already in ISO format
    nrgFile.seekg(16 * 2048);
    char isoBuf[8];
    nrgFile.read(isoBuf, 8);
    
    if (memcmp(isoBuf, "\x01" "CD001" "\x01\x00", 8) == 0) {
        return false;  // Already an ISO, no conversion needed
    }

    // Reopen file for conversion to avoid multiple seekg operations
    nrgFile.clear();
    nrgFile.seekg(307200, std::ios::beg);  // Skipping the header section

    std::ofstream isoFile(outputFile, std::ios::binary);
    if (!isoFile) {
        return false;
    }

    // Preallocate output file by setting its size
    isoFile.seekp(nrgFileSize - 1);  // Move the write pointer to the end of the file
    isoFile.write("", 1);  // Write a dummy byte to allocate space
    isoFile.seekp(0, std::ios::beg);  // Reset write pointer to the beginning of the file

    // Buffer for reading and writing
    constexpr size_t BUFFER_SIZE = 8 * 1024 * 1024;  // 8MB buffer
    std::vector<char> buffer(BUFFER_SIZE);

    // Initialize completedBytes to 0 if provided
    if (completedBytes) {
        *completedBytes = 0;
    }

    // Read and write in chunks
    while (nrgFile) {
        nrgFile.read(buffer.data(), BUFFER_SIZE);
        std::streamsize bytesRead = nrgFile.gcount();
        
        if (bytesRead > 0) {
            isoFile.write(buffer.data(), bytesRead);

            // Update completedBytes if the pointer is not null
            if (completedBytes) {
                completedBytes->fetch_add(bytesRead, std::memory_order_relaxed);
            }
        }
    }

    return true;
}
