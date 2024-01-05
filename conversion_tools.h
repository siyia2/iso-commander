// conversion_tools.h

#ifndef CONVERSION_TOOLS_H
#define CONVERSION_TOOLS_H

#include <sys/mount.h>
#include <chrono>
#include <dirent.h>
#include <execution>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <regex>
#include <set>
#include <sys/types.h>
#include <sys/stat.h>
#include <thread>
#include <unordered_set>
#include <vector>
#include <iterator>
#include <cstring>


// Function prototypes

// BIN/IMG CONVERSION
std::vector<std::string> findBinImgFiles(std::vector<std::string>& paths, const std::function<void(const std::string&, const std::string&)>& callback);
void processFilesInRange(int start, int end);
void convertBINToISO(const std::string& inputPath);
void select_and_convert_files_to_iso();
void processInputBin(const std::string& input, const std::vector<std::string>& fileList);
bool isCcd2IsoInstalled();
void printFileListBin(const std::vector<std::string>& fileList);

// MDF/MDS CONVERSION
std::future<std::vector<std::string>> getSelectedFilesMDF(const std::vector<int>& selectedIndices, const std::vector<std::string>& fileList);
std::future<std::pair<std::vector<int>, std::vector<std::string>>> processInputMDF(const std::string& input, int maxIndex);
std::vector<std::string> findMdsMdfFiles(const std::vector<std::string>& paths, const std::function<void(const std::string&, const std::string&)>& callback);
std::vector<std::future<std::pair<std::vector<int>, std::vector<std::string>>>> parseUserInputMultithreaded(const std::vector<std::string>& inputs, int maxIndex);
void convertMDFToISO(const std::string& inputPath);
void convertMDFsToISOs(const std::vector<std::string>& inputPaths);
void select_and_convert_files_to_iso_mdf();
bool isMdf2IsoInstalled();
void printFileListMdf(const std::vector<std::string>& fileList);
#endif // CONVERSION_TOOLS_H
