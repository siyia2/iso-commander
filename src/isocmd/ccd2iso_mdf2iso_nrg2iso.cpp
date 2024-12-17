#include "../headers.h"

// Special thanks to the original authors of the conversion tools:

// Salvatore Santagati (mdf2iso).
// Grégory Kokanosky (nrg2iso).
// Danny Kurniawan and Kerry Harris (ccd2iso).

// Note: Their original code has been modernized and ported to C++.

// MDF2ISO

bool convertMdfToIso(const std::string& mdfPath, const std::string& isoPath) {
    std::ifstream mdfFile(mdfPath, std::ios::binary);
    std::ofstream isoFile(isoPath, std::ios::binary);

    if (!mdfFile.is_open() || !isoFile.is_open()) {
        return false;
    }

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

    // Pre-allocate the ISO file to optimize file write performance
    isoFile.seekp(source_length * sector_data);
    isoFile.put(0); // Write a dummy byte to allocate space
    isoFile.seekp(0); // Reset the pointer to the beginning of the file

    // Buffer for reading and writing
    const size_t bufferSize = 8 * 1024 * 1024; // 8 MB
    std::vector<char> buffer(bufferSize);
    size_t bufferIndex = 0;

    while (source_length > 0) {
        // Read sector data
        mdfFile.seekg(static_cast<std::streamoff>(seek_head), std::ios::cur);
        if (!mdfFile.read(buffer.data() + bufferIndex, sector_data)) {
            return false;
        }
        mdfFile.seekg(static_cast<std::streamoff>(seek_ecc), std::ios::cur);

        bufferIndex += sector_data;

        // Write full buffer if filled
        if (bufferIndex >= bufferSize) {
            if (!isoFile.write(buffer.data(), bufferIndex)) {
                return false;
            }
            bufferIndex = 0;
        }

        --source_length;
    }

    // Write any remaining data in the buffer
    if (bufferIndex > 0) {
        if (!isoFile.write(buffer.data(), bufferIndex)) {
            return false;
        }
    }

    return true;
}


// CCD2ISO

const size_t DATA_SIZE = 2048;
const size_t BUFFER_SIZE = 8 * 1024 * 1024;  // 8 MB buffer

struct __attribute__((packed)) CcdSectheaderSyn {
    uint8_t data[12];
};

struct __attribute__((packed)) CcdSectheaderHeader {
    uint8_t sectaddr_min, sectaddr_sec, sectaddr_frac;
    uint8_t mode;
};

struct __attribute__((packed)) CcdSectheader {
    CcdSectheaderSyn syn;
    CcdSectheaderHeader header;
};

struct __attribute__((packed)) CcdSector {
    CcdSectheader sectheader;
    union {
        struct {
            uint8_t data[DATA_SIZE];
            uint8_t edc[4];
            uint8_t unused[8];
            uint8_t ecc[276];
        } mode1;
        struct {
            uint8_t sectsubheader[8];
            uint8_t data[DATA_SIZE];
            uint8_t edc[4];
            uint8_t ecc[276];
        } mode2;
    } content;
};


bool convertCcdToIso(const std::string& ccdPath, const std::string& isoPath) {
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
    }

    return true;
}


// NRG2ISO

bool convertNrgToIso(const std::string& inputFile, const std::string& outputFile) {
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

    // Read and write in chunks
    while (nrgFile) {
        nrgFile.read(buffer.data(), BUFFER_SIZE);
        std::streamsize bytesRead = nrgFile.gcount();
        
        if (bytesRead > 0) {
            isoFile.write(buffer.data(), bytesRead);
        }
    }

    return true;
}
