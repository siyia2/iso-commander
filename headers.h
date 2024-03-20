#ifndef HEADERS_H
#define HEADERS_H

#include <algorithm>
#include <chrono>
#include <execution>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <omp.h>
#include <queue>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

// Global variable for getting the num_threads
extern unsigned int num_threads;


// Global and shared mutexes
extern std::mutex skipped_folder_count_mutex;
extern std::mutex dirs_count_mutex;
extern std::mutex cout_mutex;

// Function prototypes

// Case modes

// General
std::string sentenceCase(const std::string& string);
std::string to_pascal(const std::string& string);
std::string from_pascal_case(const std::string& string);
std::string append_numbered_prefix(const std::filesystem::path& parent_path, const std::string& file_string);
std::string capitalizeFirstLetter(const std::string& string);
std::string swap_transform(const std::string& string);
std::string swapr_transform(const std::string& string);
std::string to_camel_case(const std::string& string);
std::string from_camel_case(const std::string& string);
// Files only
std::string append_numbered_prefix(const std::filesystem::path& parent_path, const std::string& file_string);
std::string remove_numbered_prefix(const std::string& file_string);
std::string append_date_seq(const std::string& file_string);
std::string remove_date_seq(const std::string& file_string);
// Mv style for folders only
void rename_folders_with_sequential_numbering(const fs::path& base_directory, std::string prefix, int& dirs_count, int& skipped_folder_special_count, int depth, bool verbose_enabled, bool skipped, bool skipped_only, bool symlinks, size_t batch_size_folders);
void rename_folders_with_sequential_numbering(const fs::path& base_directory, std::string prefix, int& dirs_count, int& skipped_folder_special_count, int depth, bool verbose_enabled, bool skipped, bool skipped_only, bool symlinks, size_t batch_size_folders);
// Simplified for folders only
std::string get_renamed_folder_name_without_numbering(const fs::path& folder_path);
std::string get_renamed_folder_name_without_date(const fs::path& folder_path);
std::string append_date_suffix_to_folder_name(const fs::path& folder_path);

// main

// General
void print_error(const std::string& error, std::ostream& os = std::cerr);
void print_verbose_enabled(const std::string& message, std::ostream& os = std::cout);
void printVersionNumber(const std::string& version);
void print_help();
// For file extensions
void rename_extension(const std::vector<fs::path>& item_paths, const std::string& case_input, bool verbose_enabled, int& files_count, size_t batch_size, bool symlinks, int& skipped_file_count, bool skipped, bool skipped_only);
void batch_rename_extension(const std::vector<std::pair<fs::path, fs::path>>& data, bool verbose_enabled, int& files_count, bool skipped_only);
void rename_extension_path(const std::vector<std::string>& paths, const std::string& case_input, bool verbose_enabled, int depth, int& files_count, size_t batch_size_files, bool symlinks, int& skipped_file_count, bool skipped, bool skipped_only, bool non_interactive);
// For file renaming
void rename_file(const fs::path& item_path, const std::string& case_input, bool is_directory, bool verbose_enabled, bool transform_dirs, bool transform_files, int& files_count, int& dirs_count, size_t batch_size_files, bool symlinks, int& skipped_file_count, int& skipped_folder_count, bool skipped, bool skipped_only);
void rename_batch(const std::vector<std::pair<fs::path, std::string>>& data, bool verbose_enabled, int& files_count, int& dirs_count, bool skipped_only);
// For folder renaming
void rename_directory(const fs::path& directory_path, const std::string& case_input, bool rename_parents, bool verbose_enabled, bool transform_dirs, bool transform_files, int& files_count, int& dirs_count, int depth, size_t batch_size_files, size_t batch_size_folders, bool symlinks, int& skipped_file_count, int& skipped_folder_count, int& skipped_folder_special_count, bool skipped, bool skipped_only, bool isFirstRun, bool& special);
void rename_path(const std::vector<std::string>& paths, const std::string& case_input, bool rename_parents, bool verbose_enabled, bool transform_dirs, bool transform_files, int depth, int files_count, int dirs_count, size_t batch_size_files, size_t batch_size_folders, bool symlinks, int skipped_file_count, int skipped_folder_count, int skipped_folder_special_count, bool skipped, bool skipped_only, bool isFirstRun, bool non_interactive, bool special);

#endif // HEADERS_H
