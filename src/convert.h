// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CONVERT_H
#define CONVERT_H

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <string>

/**
 * @brief Selects which disk-image conversion backend a job should use.
 *
 * Exactly one flag is expected to be set per conversion. When all flags are
 * @c false, the CCD backend is selected (mirroring the historical fallthrough
 * behavior of the internal `runConversion` dispatcher).
 *
 * @note This struct is intentionally dependency-free so it can be included
 *       from any translation unit without pulling in project globals.
 */
struct ConversionMode {
    bool mdf = false; ///< @c true selects the MDF/MDS backend (@c convertMdfToIso).
    bool nrg = false; ///< @c true selects the NRG backend (@c convertNrgToIso).
    bool chd = false; ///< @c true selects the CHD backend (@c convertChdToIso).
    bool daa = false; ///< @c true selects the DAA/GBI backend (@c convertDaaToIso).
};

/**
 * @brief A single source-to-destination conversion request.
 *
 * Holds the input image path and the target ISO path. Callers may leave
 * @ref outputPath empty and let the batch driver derive it (see
 * `deriveOutputPath`); if a non-empty value is supplied it is used verbatim.
 */
struct ConversionJob {
    std::string inputPath;  ///< Absolute or relative path to the source disk image.
    std::string outputPath; ///< Target ISO path; empty means "derive from @ref inputPath".
};

/**
 * @brief Converts a CCD/CUE-based image set into a plain ISO.
 *
 * @param ccdPath         Path to the source CCD (or associated cue/bin) descriptor.
 * @param isoPath         Destination ISO path. Any existing file is overwritten.
 * @param completedBytes  Optional atomic counter incremented by the number of
 *                        bytes processed; may be @c nullptr when progress
 *                        reporting is not required.
 *
 * @return @c true on success; @c false if the source is unreadable, the
 *         backend reports an error, or the operation is cancelled.
 *
 * @note Thread-safety: safe to invoke concurrently for distinct (ccdPath, isoPath)
 *       pairs; concurrent calls sharing either path are the caller's responsibility.
 */
bool convertCcdToIso(const std::string& ccdPath, const std::string& isoPath, std::atomic<size_t>* completedBytes);

/**
 * @brief Converts a CHD (MAME Compressed Hunks of Data) image into a plain ISO.
 *
 * @param chdPath         Path to the source @c .chd file.
 * @param isoPath         Destination ISO path. Any existing file is overwritten.
 * @param completedBytes  Optional atomic counter incremented by the number of
 *                        bytes processed; may be @c nullptr.
 *
 * @return @c true on success; @c false on read error, decode failure, or
 *         cancellation.
 */
bool convertChdToIso(const std::string& chdPath, const std::string& isoPath, std::atomic<size_t>* completedBytes);

/**
 * @brief Converts a DAA/GBI image into a plain ISO.
 *
 * @param inputFile       Path to the source @c .daa or @c .gbi file.
 * @param outputFile      Destination ISO path. Any existing file is overwritten.
 * @param completedBytes  Optional atomic counter incremented by the number of
 *                        bytes processed; may be @c nullptr.
 *
 * @return @c true on success; @c false on read error, decode failure, or
 *         cancellation.
 */
bool convertDaaToIso(const std::string &inputFile, const std::string &outputFile, std::atomic<size_t> *completedBytes);

/**
 * @brief Converts an MDF/MDS image pair into a plain ISO.
 *
 * @param mdfPath         Path to the source @c .mdf media file; the sibling
 *                        @c .mds descriptor is located automatically.
 * @param isoPath         Destination ISO path. Any existing file is overwritten.
 * @param completedBytes  Optional atomic counter incremented by the number of
 *                        bytes processed; may be @c nullptr.
 *
 * @return @c true on success; @c false on read error, descriptor mismatch, or
 *         cancellation.
 */
bool convertMdfToIso(const std::string& mdfPath, const std::string& isoPath, std::atomic<size_t>* completedBytes);

/**
 * @brief Converts an NRG (Nero) image into a plain ISO.
 *
 * @param inputFile       Path to the source @c .nrg file.
 * @param outputFile      Destination ISO path. Any existing file is overwritten.
 * @param completedBytes  Optional atomic counter incremented by the number of
 *                        bytes processed; may be @c nullptr.
 *
 * @return @c true on success; @c false on read error, invalid NRG layout, or
 *         cancellation.
 */
bool convertNrgToIso(const std::string& inputFile, const std::string& outputFile, std::atomic<size_t>* completedBytes);

#endif // CONVERT_H
