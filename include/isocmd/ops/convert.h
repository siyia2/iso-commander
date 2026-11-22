// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CONVERT_H
#define CONVERT_H

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

/**
 * @brief Shared dependencies and output collection for a conversion batch.
 *
 * Supplies caller-owned counters and optional successful-output collection.
 * All pointed-to objects must outlive the conversion batch.
 *
 * @note When @ref successfulOutputPaths is non-null, @ref outPathsMutex
 *       must also be non-null.
 */
struct ConversionBatchData {
    std::atomic<size_t>* completedBytes = nullptr;
    std::atomic<size_t>* completedTasks = nullptr;
    std::atomic<size_t>* failedTasks = nullptr;

    std::vector<std::string>* successfulOutputPaths = nullptr;
    std::mutex* outPathsMutex = nullptr;
};

/**
 * @brief Selects which disk-image conversion backend a job should use.
 *
 * Exactly one flag is expected to be set per conversion. If several are set,
 * the dispatcher picks the first in the order MDF, NRG, CHD, DAA. When all
 * flags are @c false, the CCD backend is selected (mirroring the historical
 * fallthrough behavior of the internal `runConversion` dispatcher).
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
 * @brief Batch-converts disk images into ISO format.
 *
 * For each source path the ISO is written next to the source with the
 * extension replaced by `.iso`. Missing or unreadable sources are recorded as
 * failures, and jobs whose destination ISO already exists are skipped (counted
 * as completed). Iteration stops early when the global cancellation flag is set.
 *
 * @param imageFiles Source image paths to convert.
 * @param mode       Selects the conversion backend.
 * @param batchData  Caller-owned counters and optional output collection.
 *                   @c completedTasks and @c failedTasks must be non-null.
 */
void convertToISO(
    const std::vector<std::string>& imageFiles,
    const ConversionMode& mode,
    const ConversionBatchData& batchData);

/**
 * @brief Converts a CCD/CUE-based image set into a plain ISO.
 *
 * @param ccdPath         Path to the source CCD (or associated cue/bin) descriptor.
 * @param isoPath         Destination ISO path. The backend itself overwrites an
 *                        existing file; @ref convertToISO skips such jobs beforehand.
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
 * @param isoPath         Destination ISO path. The backend overwrites an existing file;
 *                        @ref convertToISO skips such jobs beforehand.
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
 * @param outputFile      Destination ISO path. The backend overwrites an existing file;
 *                        @ref convertToISO skips such jobs beforehand.
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
 * @param isoPath         Destination ISO path. The backend overwrites an existing file;
 *                        @ref convertToISO skips such jobs beforehand.
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
 * @param outputFile      Destination ISO path. The backend overwrites an existing file;
 *                        @ref convertToISO skips such jobs beforehand.
 * @param completedBytes  Optional atomic counter incremented by the number of
 *                        bytes processed; may be @c nullptr.
 *
 * @return @c true on success; @c false on read error, invalid NRG layout, or
 *         cancellation.
 */
bool convertNrgToIso(const std::string& inputFile, const std::string& outputFile, std::atomic<size_t>* completedBytes);

#endif // CONVERT_H
