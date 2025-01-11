#ifndef MDF_H
#define MDF_H

struct MdfTypeInfo {
    size_t seek_ecc;
    size_t sector_size;
    size_t sector_data;
    size_t seek_head;

    MdfTypeInfo() : seek_ecc(0), sector_size(0), sector_data(0), seek_head(0) {}

    bool determineMdfType(std::ifstream& mdfFile) {
        char buf[12];
        mdfFile.seekg(0);
        if (!mdfFile.read(buf, 12)) {
            return false;
        }

        if (std::memcmp("\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00", buf, 12) == 0) {
            mdfFile.seekg(2352);
            if (!mdfFile.read(buf, 12)) {
                return false;
            }

            if (std::memcmp("\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00", buf, 12) == 0) {
                // Type 1: 2352-byte sectors with 2048-byte user data
                seek_ecc = 288;
                sector_size = 2352;
                sector_data = 2048;
                seek_head = 16;
            } else {
                // Type 2: 2448-byte sectors with 2048-byte user data
                seek_ecc = 384;
                sector_size = 2448;
                sector_data = 2048;
                seek_head = 16;
            }
        } else {
            // Type 3: 2448-byte sectors with 2352-byte user data
            seek_head = 0;
            sector_size = 2448;
            seek_ecc = 96;
            sector_data = 2352;
        }

        return true;
    }
};

#endif // MDF_H
