/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef CHD_COMMON_H
#define CHD_COMMON_H

#include <chd.h>

struct ChdFileDeleter {
    void operator()(chd_file* f) const noexcept {
        if (f) chd_close(f);
    }
};
using ChdFilePtr = std::unique_ptr<chd_file, ChdFileDeleter>;

#endif
