// conversion_tools.h

#ifndef CONVERSION_TOOLS_H
#define CONVERSION_TOOLS_H

#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <omp.h>
#include <regex>
#include <set>
#include <sys/types.h>
#include <sys/stat.h>
#include <thread>
#include <unordered_set>
#include <vector>


// Function prototypes

// BIN/IMG CONVERSION
std::string chooseFileToConvert(const std::vector<std::string>& files);
std::vector<std::string> findBinImgFiles(const std::string& directory);
void convertBINToISO(const std::string& inputPath);
void convertBINsToISOs(const std::vector<std::string>& inputPaths, int numThreads);
void processFilesInRange(int start, int end);
void convertBINsToISOs();
void select_and_convert_files_to_iso();
void processInputBin(const std::string& input, const std::vector<std::string>& fileList);
bool isCcd2IsoInstalled();
void printFileListBin(const std::vector<std::string>& fileList);

// MDF/MDS CONVERSION
std::vector<std::string> getSelectedFiles(const std::vector<int>& selectedIndices, const std::vector<std::string>& fileList);
std::vector<int> parseUserInput(const std::string& input, int maxIndex);
std::vector<std::string> findMdsMdfFiles(const std::string& directory);
void convertMDFToISO(const std::string& inputPath);
void convertMDFsToISOs(const std::vector<std::string>& inputPaths, int numThreads);
void processMDFFilesInRange(int start, int end);
void select_and_convert_files_to_iso_mdf();
void processMdfMdsFilesInRange(const std::vector<std::string>& mdfMdsFiles, int start, int end);
bool isMdf2IsoInstalled();
void printFileListMdf(const std::vector<std::string>& fileList);
#endif // CONVERSION_TOOLS_H
