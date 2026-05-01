/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef CHD_COMMON_H
#define CHD_COMMON_H

// C++ Standard Library Headers
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

// Third-Party Library Headers
#include <chd.h>


// C / System Headers
#include <fcntl.h>
#include <sys/mman.h>

namespace fs = std::filesystem;

/**
 * @brief Custom deleter for CHD file handles
 * 
 * This deleter is designed to be used with std::unique_ptr to automatically
 * manage the lifetime of CHD file handles. It ensures that chd_close() is
 * called when the unique_ptr goes out of scope.
 * 
 * @note This deleter is noexcept and will not throw exceptions during cleanup.
 */
struct ChdFileDeleter {
    /**
     * @brief Closes a CHD file handle
     * 
     * @param f Pointer to the CHD file handle to close. If nullptr, the
     *          function does nothing.
     */
    void operator()(chd_file* f) const noexcept {
        if (f) chd_close(f);
    }
};

/**
 * @brief RAII smart pointer type for CHD file handles
 * 
 * This type alias defines a std::unique_ptr that automatically manages
 * CHD file handles using ChdFileDeleter. It provides exception-safe
 * lifetime management for CHD resources.
 * 
 * Usage example:
 * @code
 * chd_file* raw_handle;
 * if (chd_open("game.chd", CHD_OPEN_READ, NULL, &raw_handle) == CHDERR_SUCCESS) {
 *     ChdFilePtr handle(raw_handle);
 *     // Use handle.get() to access the raw pointer
 *     // Handle will be automatically closed when it goes out of scope
 * }
 * @endcode
 * 
 * @note The underlying CHD file handle can be accessed via .get() method
 * @see ChdFileDeleter
 */
using ChdFilePtr = std::unique_ptr<chd_file, ChdFileDeleter>;

#endif // CHD_COMMON_H
