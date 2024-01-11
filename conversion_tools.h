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
#include <queue>
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

// General

bool fileExistsConversions(const std::string& fullPath);

// BIN/IMG CONVERSION
bool endsWith(const std::string& fullString, const std::string& ending);
std::string toLower(const std::string& str);
std::vector<std::string> findBinImgFiles(std::vector<std::string>& paths, const std::function<void(const std::string&, const std::string&)>& callback);
void convertBINToISO(const std::string& inputPath);
void select_and_convert_files_to_iso();
void processInputBin(const std::string& input, const std::vector<std::string>& fileList);
bool isCcd2IsoInstalled();
void printFileListBin(const std::vector<std::string>& fileList);

// MDF/MDS CONVERSION
std::vector<std::string> findMdsMdfFiles(const std::vector<std::string>& paths, const std::function<void(const std::string&, const std::string&)>& callback);
void processInputMDF(const std::string& input, const std::vector<std::string>& fileList);
void convertMDFToISO(const std::string& inputPath);
void select_and_convert_files_to_iso_mdf();
bool isMdf2IsoInstalled();
void printFileListMdf(const std::vector<std::string>& fileList);
#endif // CONVERSION_TOOLS_H
