// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CONVERT
#define CONVERT

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <string>

// CCD2ISO
bool convertCcdToIso(const std::string& ccdPath, const std::string& isoPath, std::atomic<size_t>* completedBytes);

// CHD2ISO
bool convertChdToIso(const std::string& chdPath, const std::string& isoPath, std::atomic<size_t>* completedBytes);

// DAA2ISO
bool convertDaaToIso(const std::string &inputFile, const std::string &outputFile, std::atomic<size_t> *completedBytes);

// MDF2ISO
bool convertMdfToIso(const std::string& mdfPath, const std::string& isoPath, std::atomic<size_t>* completedBytes);

// NRG2ISO
bool convertNrgToIso(const std::string& inputFile, const std::string& outputFile, std::atomic<size_t>* completedBytes);

#endif // CONVERT
