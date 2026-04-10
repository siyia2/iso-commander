#ifndef DAA2ISO_H
#define DAA2ISO_H

#include <cstdint>

#pragma pack(4)
struct daa_t {
    uint8_t  sign[16];
    uint32_t size_offset;
    uint32_t version;
    uint32_t data_offset;
    uint32_t b1;
    uint32_t b0;
    uint32_t chunksize;
    uint64_t isosize;
    uint64_t daasize;
    uint8_t  hdata[16];
    uint32_t crc;
};
#pragma pack()

// Byte-swap helpers (inline)
inline void swap32_if_be(uint32_t* n, int endian) {
    if (!endian) return;
    uint32_t t = *n;
    *n = ((t & 0xff000000) >> 24) | ((t & 0x00ff0000) >> 8) |
         ((t & 0x0000ff00) << 8)  | ((t & 0x000000ff) << 24);
}

inline void swap64_if_be(uint64_t* n, int endian) {
    if (!endian) return;
    uint64_t t = *n;
    *n = ((t & 0xff00000000000000ULL) >> 56) |
         ((t & 0x00ff000000000000ULL) >> 40) |
         ((t & 0x0000ff0000000000ULL) >> 24) |
         ((t & 0x000000ff00000000ULL) >> 8)  |
         ((t & 0x00000000ff000000ULL) << 8)  |
         ((t & 0x0000000000ff0000ULL) << 24) |
         ((t & 0x000000000000ff00ULL) << 40) |
         ((t & 0x00000000000000ffULL) << 56);
}

inline void swap_daa_if_be(daa_t* d, int endian) {
    if (!endian) return;
    swap32_if_be(&d->size_offset, 1);
    swap32_if_be(&d->version, 1);
    swap32_if_be(&d->data_offset, 1);
    swap32_if_be(&d->b1, 1);
    swap32_if_be(&d->b0, 1);
    swap32_if_be(&d->chunksize, 1);
    swap64_if_be(&d->isosize, 1);
    swap64_if_be(&d->daasize, 1);
    swap32_if_be(&d->crc, 1);
}

// Helper to get uncompressed ISO size from a DAA file
static uint64_t getDaaIsoSize(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;

    daa_t daa;
    if (fread(&daa, 1, sizeof(daa), f) != sizeof(daa)) {
        fclose(f);
        return 0;
    }
    fclose(f);

    // Determine host endianness (0 = little-endian, no swap)
    int endian = 1;
    if (*(char*)&endian) endian = 0;
    swap_daa_if_be(&daa, endian);   // using the same function from daa2iso

    // Check signature (basic validation)
    if (strncmp((char*)daa.sign, "DAA", 16) != 0 &&
        strncmp((char*)daa.sign, "GBI", 16) != 0) {
        return 0;
    }
    return daa.isosize;
}

#endif // DAA2ISO_H
