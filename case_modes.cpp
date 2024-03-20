#include "headers.h"

// Necessary redefinition of global and shared mutexes
std::mutex cout_mutex;
std::mutex dirs_count_mutex;
std::mutex skipped_folder_count_mutex;

// Separate string operations

// for Files&Dirs

// Function to reaname to sentenceCase
std::string sentenceCase(const std::string& string) {
    if (string.empty()) return string; // Handling empty string case
    
    std::stringstream result;
    bool newWord = true;
    
    for (char c : string) {
        if (newWord && std::isalpha(c)) {
            result << static_cast<char>(std::toupper(c));
            newWord = false;
        } else {
            result << static_cast<char>(std::tolower(c));
        }
        if (std::isspace(c) || c == '.') { // Consider a new word after a space or period
            newWord = true;
        }
    }
    
    return result.str();
}


// Function to reaname to titlerCase
std::string capitalizeFirstLetter(const std::string& input) {
    if (input.empty()) return input; // Handling empty string case
    
    std::stringstream result;
    bool first = true;
    
    for (char c : input) {
        if (first && std::isalpha(c)) {
            result << static_cast<char>(std::toupper(c));
            first = false;
        } else {
            result << static_cast<char>(std::tolower(c)); // Convert to lowercase
        }
    }
    
    return result.str();
}


// Function to reaname to swapCase
std::string swap_transform(const std::string& string) {
    std::stringstream transformed;
    bool capitalize = false; // Start by capitalizing
    bool inFolderName = true; // Start within folder name
    size_t folderDelimiter = string.find_last_of("/\\"); // Find the last folder delimiter
    size_t length = string.length(); // Cache the length of the string string

    for (size_t i = 0; i < length; ++i) {
        char c = string[i];
        if (i < folderDelimiter || folderDelimiter == std::string::npos) { // Ignore transformation after folder delimiter
            if (inFolderName) {
                transformed << static_cast<char>(std::toupper(c)); // Capitalize first character in folder name
                inFolderName = false; // Exit folder name after first character
            } else {
                if (std::isalpha(c)) {
                    if (capitalize) {
                        transformed << static_cast<char>(std::toupper(c));
                    } else {
                        transformed << static_cast<char>(std::tolower(c));
                    }
                    capitalize = !capitalize; // Toggle between upper and lower case
                } else {
                    transformed << c; // Keep non-alphabetic characters unchanged
                }
            }
        } else {
            transformed << c; // Keep characters after the folder delimiter unchanged
        }
    }

    return transformed.str();
}

// Function to reaname to swaprCase
std::string swapr_transform(const std::string& string) {
    std::stringstream transformed;
    bool capitalize = false; // Start by capitalizing
    bool inFolderName = true; // Start within folder name
    size_t folderDelimiter = string.find_last_of("/\\"); // Find the last folder delimiter
    size_t length = string.length(); // Cache the length of the string string

    for (size_t i = 0; i < length; ++i) {
        char c = string[i];
        if (i < folderDelimiter || folderDelimiter == std::string::npos) { // Ignore transformation after folder delimiter
            if (inFolderName) {
                transformed << static_cast<char>(std::tolower(c)); // lower first character in folder name
                inFolderName = false; // Exit folder name after first character
            } else {
                if (std::isalpha(c)) {
                    if (capitalize) {
                        transformed << static_cast<char>(std::tolower(c));
                    } else {
                        transformed << static_cast<char>(std::toupper(c));
                    }
                    capitalize = !capitalize; // Toggle between lower and upper case
                } else {
                    transformed << c; // Keep non-alphabetic characters unchanged
                }
            }
        } else {
            transformed << c; // Keep characters after the folder delimiter unchanged
        }
    }

    return transformed.str();
}


// Function to rename to pascalCase
std::string to_camel_case(const std::string& string) {
    bool hasUpperCase = false;
    bool hasSpace = false;

    for (char c : string) {
        if (std::isupper(c)) {
            hasUpperCase = true;
        } else if (c == ' ') {
            hasSpace = true;
        }
        if (c == '.') {
            // Stop processing after encountering a period
            break;
        }
    }

    if (!hasSpace && hasUpperCase) {
        return string;
    }

    std::string result;
    result.reserve(string.size() + 10); // Adjust the reserve size as needed

    bool capitalizeNext = false;
    bool afterDot = false;

    for (char c : string) {
        if (c == '.') {
            afterDot = true;
        }
        if (afterDot) {
            result += c;
        } else {
            if (std::isalpha(c)) {
                result += capitalizeNext ? toupper(c) : tolower(c); // Use toupper directly
                capitalizeNext = false;
            } else if (c == ' ') {
                capitalizeNext = true;
            } else {
                result += c;
            }
        }
    }

    return result;
}


// Function to reverse camelCase
std::string from_camel_case(const std::string& string) {
    std::string result;
    result.reserve(string.size() + std::count_if(string.begin(), string.end(), ::isupper)); // Reserve space for the result string

    for (char c : string) {
        if (std::isupper(c)) {
            result += ' ';
            result += std::tolower(c);
        } else {
            result += c;
        }
    }

    return result;
}

// Function to rename to pascalCase
std::string to_pascal(const std::string& string) {
    bool hasUpperCase = false;
    bool hasSpace = false;

    for (char c : string) {
        if (std::isupper(c)) {
            hasUpperCase = true;
        } else if (c == ' ') {
            hasSpace = true;
        }
        if (c == '.') {
            // Stop processing after encountering a period
            break;
        }
    }

    if (!hasSpace && hasUpperCase) {
        return string;
    }

    std::string result;
    result.reserve(string.size() + 10); // Adjust the reserve size as needed

    bool capitalizeNext = true; // Start with true for PascalCase
    bool afterDot = false;

    for (char c : string) {
        if (c == '.') {
            afterDot = true;
        }
        if (afterDot) {
            result += c;
            capitalizeNext = true; // Reset for new word after period
        } else {
            if (std::isalpha(c)) {
                result += capitalizeNext ? toupper(c) : tolower(c);
                capitalizeNext = false;
            } else if (c == ' ') {
                capitalizeNext = true;
            } else {
                result += c;
            }
        }
    }

    return result;
}


// Function to reverse pascalCase
std::string from_pascal_case(const std::string& string) {
    std::string result;
    result.reserve(string.size()); // Reserve space for the result string

    for (size_t i = 0; i < string.size(); ++i) {
        char c = string[i];
        if (std::isupper(c)) {
            // If the current character is uppercase and it's not the first character,
            // and the previous character was lowercase, add a space before appending the uppercase character
            if (i != 0 && std::islower(string[i - 1])) {
                result += ' ';
            }
            result += c;
        } else {
            result += std::tolower(c);
        }
    }

    return result;
}


// Function to remove sequential numbering from folder name
std::string get_renamed_folder_name_without_numbering(const fs::path& folder_path) {
    std::string folder_name = folder_path.filename().string();

    // Find the position of the first non-zero digit
    size_t first_non_zero = folder_name.find_first_not_of('0');

    // Check if the folder name starts with a sequence of digits followed by an underscore
    size_t underscore_pos = folder_name.find('_', first_non_zero);
    if (underscore_pos != std::string::npos && folder_name.find_first_not_of("0123456789", first_non_zero) == underscore_pos) {
        // Extract the original name without the numbering
        std::string original_name = folder_name.substr(underscore_pos + 1);
        return original_name;
    }

    // No sequential numbering found, return original name
    return folder_name;
}


// Function to remove date suffix from folder name
std::string get_renamed_folder_name_without_date(const fs::path& folder_path) {
    std::string folder_name = folder_path.filename().string();

    // Check if the folder name ends with the date suffix format "_YYYYMMDD"
    if (folder_name.size() < 9 || folder_name.substr(folder_name.size() - 9, 1) != "_")
        return folder_name; // No date suffix found, return original name

    bool is_date_suffix = true;
    for (size_t i = folder_name.size() - 8; i < folder_name.size(); ++i) {
        if (!std::isdigit(folder_name[i])) {
            is_date_suffix = false;
            break;
        }
    }

    if (!is_date_suffix)
        return folder_name; // Not a valid date suffix, return original name

    // Remove the date suffix from the folder name
    std::string new_folder_name = folder_name.substr(0, folder_name.size() - 9);

    return new_folder_name;
}

// Function to append date suffix to folder name
std::string append_date_suffix_to_folder_name(const fs::path& folder_path) {
    std::string folder_name = folder_path.filename().string();

    // If the folder name is empty, return an empty string
    if (folder_name.empty())
        return "";

    // Check if the folder name already ends with a date suffix
    if (folder_name.size() >= 9 && folder_name.substr(folder_name.size() - 9, 1) == "_" &&
        folder_name.substr(folder_name.size() - 8, 8).find_first_not_of("0123456789") == std::string::npos)
        return folder_name; // If it does, do not rename

    // Find the last underscore character in the folder name, excluding numeric prefix
    size_t last_underscore_pos = folder_name.find_last_of('_', folder_name.find_first_not_of("0123456789"));

    // Check if there is an underscore and if the substring after it matches the date format
    if (last_underscore_pos != std::string::npos &&
        folder_name.size() - last_underscore_pos == 9 &&
        folder_name.substr(last_underscore_pos + 1, 8).find_first_not_of("0123456789") == std::string::npos)
        return folder_name; // If it does, do not rename

    // Get the current date in YYYYMMDD format
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm = *std::localtime(&now_time_t);
    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y%m%d");
    std::string date_suffix = "_" + oss.str();

    // Append the date suffix to the folder name
    std::string new_folder_name = folder_name + date_suffix;

    return new_folder_name;
}


// For Files

// Function to add sequencial numbering to files
std::string append_numbered_prefix(const std::filesystem::path& parent_path, const std::string& file_string) {
    static std::unordered_map<std::filesystem::path, int> counter_map;
    
    // Initialize counter if not already initialized
    if (counter_map.find(parent_path) == counter_map.end()) {
        // Find the highest existing numbered file
        int max_counter = 0;
        std::unordered_set<int> existing_numbers;

        for (const auto& entry : std::filesystem::directory_iterator(parent_path)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (!filename.empty() && std::isdigit(filename[0])) {
                    int number = std::stoi(filename.substr(0, filename.find('_')));
                    existing_numbers.insert(number);
                    if (number > max_counter) {
                        max_counter = number;
                    }
                }
            }
        }

        // Find the first gap in the sequence of numbers
        int gap = 1;
        while (existing_numbers.find(gap) != existing_numbers.end()) {
            gap++;
        }

        counter_map[parent_path] = gap - 1; // Initialize counter with the first gap
    }

    int& counter = counter_map[parent_path];

    // Check if file_string starts with a number followed by an underscore
    if (!file_string.empty() && std::isdigit(file_string[0])) {
        size_t underscore_pos = file_string.find('_');
        if (underscore_pos != std::string::npos && underscore_pos > 0 && std::all_of(file_string.begin(), file_string.begin() + underscore_pos, ::isdigit)) {
            return file_string; // Return directly if file_string starts with a numbered prefix
        }
    }

    std::ostringstream oss;
    counter++; // Increment counter before using its current value
    oss << std::setfill('0') << std::setw(3) << counter; // Width set to 3 for leading zeros up to three digits

    oss << "_" << file_string;

    return oss.str();
}


// Function to remove sequencial numbering from files
std::string remove_numbered_prefix(const std::string& file_string) {
    size_t pos = file_string.find_first_not_of("0123456789");

    // Check if the filename is already numbered and contains an underscore after numbering
    if (pos != std::string::npos && pos > 0 && file_string[pos] == '_' && file_string[pos - 1] != '_') {
        // Remove the number and the first underscore
        return file_string.substr(pos + 1);
    }

    // If the prefix contains a number at the beginning, remove it
    if (pos == 0) {
        size_t underscore_pos = file_string.find('_');
        if (underscore_pos != std::string::npos && underscore_pos > 0) {
            return file_string.substr(underscore_pos + 1);
        }
    }

    return file_string; // Return the original name if no number found or if number is not followed by an underscore
}


// Function to add current date to files
std::string append_date_seq(const std::string& file_string) {
    // Check if the filename already contains a date seq
    size_t dot_position = file_string.find_last_of('.');
    size_t underscore_position = file_string.find_last_of('_');
    if (dot_position != std::string::npos && underscore_position != std::string::npos && dot_position > underscore_position) {
        std::string date_seq = file_string.substr(underscore_position + 1, dot_position - underscore_position - 1);
        if (date_seq.size() == 8 && std::all_of(date_seq.begin(), date_seq.end(), ::isdigit)) {
            // Filename already contains a valid date seq, no need to append
            return file_string;
        }
    } else if (underscore_position != std::string::npos) {
        std::string date_seq = file_string.substr(underscore_position + 1);
        if (date_seq.size() == 8 && std::all_of(date_seq.begin(), date_seq.end(), ::isdigit)) {
            // Filename already contains a valid date seq, no need to append
            return file_string;
        }
    }

    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm* local_tm = std::localtime(&time_t_now);

    std::ostringstream oss;
    oss << std::put_time(local_tm, "%Y%m%d");
    std::string date_seq = oss.str();

    if (dot_position != std::string::npos) {
        return file_string.substr(0, dot_position) + "_" + date_seq + file_string.substr(dot_position);
    } else {
        return file_string + "_" + date_seq;
    }
}


// Function to remove date from files
std::string remove_date_seq(const std::string& file_string) {
    size_t dot_position = file_string.find_last_of('.');
    size_t underscore_position = file_string.find_last_of('_');

    if (underscore_position != std::string::npos) {
        if (dot_position != std::string::npos && dot_position > underscore_position) {
            std::string date_seq = file_string.substr(underscore_position + 1, dot_position - underscore_position - 1);
            if (date_seq.size() == 8 && std::all_of(date_seq.begin(), date_seq.end(), ::isdigit)) {
                // Valid date seq found, remove it
                return file_string.substr(0, underscore_position) + file_string.substr(dot_position);
            }
        } else {
            // No dot found after underscore, consider the substring from underscore to the end as potential date seq
            std::string date_seq = file_string.substr(underscore_position + 1);
            if (date_seq.size() == 8 && std::all_of(date_seq.begin(), date_seq.end(), ::isdigit)) {
                // Valid date seq found, remove it
                return file_string.substr(0, underscore_position);
            }
        }
    }

    // No valid date seq found, return original file_string
    return file_string;
}

 
// Folder numbering functions mv style
 
// Apply sequential folder numbering in parallel using OpenMP
void rename_folders_with_sequential_numbering(const fs::path& base_directory, std::string prefix, int& dirs_count, int& skipped_folder_special_count, int depth, bool verbose_enabled = false, bool skipped = false, bool skipped_only = false, bool symlinks = false, size_t batch_size_folders = 100) {
    int counter = 1; // Counter for immediate subdirectories
    std::vector<std::pair<fs::path, fs::path>> folders_to_rename; // Vector to store folders to be renamed
    std::vector<std::pair<fs::path, bool>> unchanged_folder_paths; // Store folder paths and their symlink status
    
    bool unnumbered_folder_exists = false; // Flag to track if at least one unnumbered folder exists
    
    // Continue recursion if the depth limit is not reached
    if (depth != 0) {
        // Decrement depth only if the depth limit is positive
        if (depth > 0)
            --depth;

        // Collect folder paths and check for unnumbered folders
        for (const auto& folder : fs::directory_iterator(base_directory)) {
            bool skip = !symlinks && fs::is_symlink(folder);
            if (folder.is_directory() && !skip) {
                std::string folder_name = folder.path().filename().string();
                unchanged_folder_paths.push_back({folder.path(), fs::is_symlink(folder)}); // Store the path and its symlink status

                size_t pos = folder_name.find('_');
                // Check if folder is unnumbered
                if (pos == std::string::npos || !std::all_of(folder_name.begin(), folder_name.begin() + pos, ::isdigit)) {
                    unnumbered_folder_exists = true;
                }

                // Remove any existing numbering from the folder name
                std::string original_name;
                if (folder_name.substr(0, 2) == "00" && pos != std::string::npos && pos > 0 && std::isdigit(folder_name[0])) {
                    original_name = folder_name.substr(pos + 1);
                } else {
                    original_name = folder_name;
                }

                // Construct the new name with sequential numbering and original name
                std::stringstream ss;
                ss << std::setw(3) << std::setfill('0') << counter << "_" << original_name;
                fs::path new_name = base_directory / (prefix.empty() ? "" : (prefix + "_")) / ss.str();

                // Add folder to the vector for batch renaming
                folders_to_rename.emplace_back(folder.path(), new_name);

                counter++; // Increment counter after each directory is processed
            }
        }

        // Only proceed with renaming if at least one unnumbered folder exists
        if (unnumbered_folder_exists) {
            // Rename folders in parallel batches
            #pragma omp parallel for shared(folders_to_rename, dirs_count) schedule(static, 1) num_threads(num_threads)
            for (size_t i = 0; i < folders_to_rename.size(); ++i) {
                const auto& folder_pair = folders_to_rename[i];
                const auto& old_path = folder_pair.first;
                const auto& new_path = folder_pair.second;

                // Move the contents of the source directory to the destination directory
                try {
                    fs::rename(old_path, new_path);
                } catch (const fs::filesystem_error& e) {
                    // Error handling
                    std::lock_guard<std::mutex> lock(dirs_count_mutex);
                    if (e.code() == std::errc::permission_denied && verbose_enabled) {
                        print_error("\033[1;91mError\033[0m: " + std::string(e.what()));
                    }
                    continue; // Skip renaming if moving fails
                }
                if (verbose_enabled && !skipped_only) {
                    if ((symlinks && fs::is_symlink(old_path)) || fs::is_symlink(new_path)) {
                        print_verbose_enabled("\033[0m\033[92mRenamed\033[0m\033[95m symlink_folder\033[0m " + old_path.string() + " to " + new_path.string(), std::cout);
                    } else {
                        print_verbose_enabled("\033[0m\033[92mRenamed\033[0m\033[94m folder\033[0m " + old_path.string() + " to " + new_path.string(), std::cout);
                    }
                }
                std::lock_guard<std::mutex> lock(dirs_count_mutex);
                ++dirs_count; // Increment dirs_count after each successful rename
            }
        } else {
            // Print folder paths that did not need renaming
            if (!unchanged_folder_paths.empty()) {
                for (const auto& folder_pair : unchanged_folder_paths) {
                    const fs::path& folder_path = folder_pair.first;
                    bool is_symlink = folder_pair.second;
                    if (verbose_enabled && skipped) {
                        if (is_symlink) {
                            print_verbose_enabled( "\033[0m\033[93mSkipped\033[0m\033[95m symlink_folder\033[0m " + folder_path.string() + " (name unchanged)", std::cout);
                        } else {
                            print_verbose_enabled("\033[0m\033[93mSkipped\033[0m\033[94m folder\033[0m " + folder_path.string() + " (name unchanged)", std::cout);
                        }
                    }
                    // Increment the counter for skipped folders
                    std::lock_guard<std::mutex> lock(skipped_folder_count_mutex);
                    ++skipped_folder_special_count;
                }
            }
        }
    }
}

// Overloaded function with default verbose_enabled = false and batch processing
void rename_folders_with_sequential_numbering(const fs::path& base_directory, int& dirs_count, int& skipped_folder_special_count, int depth, bool verbose_enabled = false, bool skipped = false, bool skipped_only = false, bool symlinks = false, size_t batch_size_folders = 100) {
    rename_folders_with_sequential_numbering(base_directory, "", dirs_count, depth, verbose_enabled, skipped, skipped_only, symlinks, batch_size_folders);
}
