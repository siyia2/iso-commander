// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <chd.h>

export module isocmd.converters.chd2iso;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- converters/chd2iso ----
/* SPDX-License-Identifier: BSD-3-Clause */



// Third-Party Library Headers

/**
 * @brief Layout and buffer size parameters for extracting sector user data from CHD hunks.
 */
struct HunkExtractConfig {
    uint32_t hunkSize;       ///< Size of each compressed/decompressed hunk in bytes.
    uint32_t rawSectorSize;  ///< Size of each raw sector within a hunk in bytes.
    uint32_t sectorsPerHunk; ///< Number of sectors contained in a single hunk.
    uint32_t userDataOffset; ///< Byte offset of user payload data inside each raw sector.
};

/**
 * @brief Per-worker execution context for parallel hunk decompression and mapping.
 */
struct HunkWorkerContext {
    const std::string& chdPath;            ///< Path to the source CHD file.
    void* mapped;                          ///< Writable memory-mapped output buffer.
    uint32_t startHunk;                    ///< Inclusive starting hunk index for the worker.
    uint32_t endHunk;                      ///< Exclusive ending hunk index for the worker.
    HunkExtractConfig config;              ///< Sector layout and hunk extraction configuration.
    std::atomic<size_t>* completedBytes = nullptr; ///< Optional atomic counter of bytes written.
    std::atomic<bool>& errorOccurred;      ///< Shared flag signaling read, open, or cancel failure.
};

/**
 * @brief Conversion parameters describing CHD layout, hunk count, and total output size.
 */
struct HunkConvertParams {
    HunkExtractConfig extract; ///< Sector layout and hunk extraction configuration.
    uint32_t totalHunks;       ///< Total number of hunks in the CHD image.
    uint64_t totalUserData;    ///< Total extracted user data size in bytes.
};

/**
 * @brief Custom deleter for CHD file handles.
 *
 * This deleter is designed to be used with std::unique_ptr to automatically
 * manage the lifetime of CHD file handles. It ensures that chd_close() is
 * called when the unique_ptr goes out of scope.
 *
 * @note This deleter is noexcept and will not throw exceptions during cleanup.
 */
struct ChdFileDeleter {
    /**
     * @brief Closes a CHD file handle.
     *
     * @param f Pointer to the CHD file handle to close. If nullptr, the
     *          function does nothing.
     */
    void operator()(chd_file* f) const noexcept {
        if (f) chd_close(f);
    }
};

/**
 * @brief RAII smart pointer type for CHD file handles.
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
 * @note The underlying CHD file handle can be accessed via .get() method.
 * @see ChdFileDeleter
 */
using ChdFilePtr = std::unique_ptr<chd_file, ChdFileDeleter>;

} // extern "C++"
