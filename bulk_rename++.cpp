#include "headers.h"

// General purpose stuff

// Batch size for file processing
constexpr int batch_size = 100;

// For verbose folder renaming
bool special = false;


// Global print functions

// Print an error message to stderr
void print_error(const std::string& error, std::ostream& os)   {
    std::lock_guard<std::mutex> lock(cout_mutex); // Ensure thread safety when writing to std::cerr
    std::cerr << error << std::endl; // Output the error message
}

// Print a message to stdout, assuming verbose mode is enabled
void print_verbose_enabled(const std::string& message, std::ostream& os) {
    std::lock_guard<std::mutex> lock(cout_mutex); // Ensure thread safety when writing to std::cout
    std::cout << message << std::endl; // Output the message
}

// Print the version number of the program
void printVersionNumber(const std::string& version) {
    std::cout << "\x1B[32mBulk-rename-plus v" << version << "\x1B[0m\n" << std::endl; // Output the version number in green color
}


// Function to print help
void print_help() {

std::cout << "\n\x1B[32mUsage: bulk_rename++ [OPTIONS] [MODE] [PATHS]\n"
          << "Renames all files and folders under the specified path(s).\n"
          << "\n"
          << "Options:\n"
          << "  -h, --help               Print help\n"
          << "  --version                Print version\n"
          << "  -v, --verbose            Enable verbose mode (optional)\n"
          << "  -fi                      Rename files exclusively (optional)\n"
          << "  -fo                      Rename folders exclusively (optional)\n"
          << "  -sym                     Handle symbolic links like regular files/folders (optional)\n"
          << "  -d  [DEPTH]              Set recursive depth level (optional)\n"
          << "  -c  [MODE]               Set Case Mode for file/folder names\n"
          << "  -cp [MODE]               Set Case Mode for file/folder/parent names\n"
          << "  -ce [MODE]               Set Case Mode for file extensions\n"
          << "\n"
          << "Available Modes:\n"
          << "Regular CASE Modes:\n"
          << "  title      Convert names to titleCase (e.g., test => Test)\n"
          << "  upper      Convert names to upperCase (e.g., Test => TEST)\n"
          << "  lower      Convert names to lowerCase (e.g., Test => test)\n"
          << "  reverse    Reverse current Case in names (e.g., Test => tEST)\n"
          << "Special CASE Modes:\n"
          << "  snake      Convert names to snakeCase (e.g., Te st => Te_st)\n"
          << "  rsnake     Reverse snakeCase in names (e.g., Te_st => Te st)\n"
          << "  kebab      Convert names to kebabCase (e.g., Te st => Te-st)\n"
          << "  rkebab     Reverse kebabCase in names (e.g., Te-st => Te st)\n"
          << "  camel      Convert names to camelCase (e.g., Te st => teSt)\n"
          << "  rcamel     Reverse camelCase in names (e.g., TeSt => te st)\n"
          << "  pascal     Convert names to pascalCase (e.g., Te st => TeSt)\n"
          << "  rpascal    Reverse pascalCase in names (e.g., TeSt => Te St)\n"
          << "  sentence   Convert names to sentenceCase (e.g., Te st => Te St)\n"
          << "Extension CASE Modes:\n"
          << "  bak        Add .bak on file extension names (e.g., Test.txt => Test.txt.bak)\n"
          << "  rbak       Remove .bak from file extension names (e.g., Test.txt.bak => Test.txt)\n"
          << "  noext      Remove file extensions (e.g., Test.txt => Test)\n"
	  << "Numerical CASE Modes:\n"
	  << "  nsequence  Append sequential numbering  (e.g., Test => 001_Test)\n"
          << "  rnsequence Remove sequential numbering from names (e.g., 001_Test => Test)\n"
	  << "  date       Append current date to names if no date pre-exists (e.g., Test => Test_20240215)\n"
	  << "  rdate      Remove date from names (e.g., Test_20240215 => Test)\n"
	  << "  rnumeric   Remove numeric characters from names (e.g., 1Te0st2 => Test)\n"
          << "Custom CASE Modes:\n"
          << "  rbra       Remove [ ] { } ( ) from names (e.g., [{Test}] => Test)\n"
          << "  roperand   Remove - + > < = * from names (e.g., =T-e+s<t> => Test)\n"
          << "  rspecial   Remove special characters from names (e.g., @T!es#$%^|&~`';?t => Test)\n"
          << "  swap       Swap upper-lower case for names (e.g., Test => TeSt)\n"
	  << "  swapr      Swap lower-upper case for names (e.g., Test => tEsT)\n"
          << "\n"
          << "Examples:\n"
          << "  bulk_rename++ -c lower [path1] [path2]...\n"
          << "  bulk_rename++ -d 0 -cp upper [path1]\n"
          << "  bulk_rename++ -v -cp upper [path1]\n"
          << "  bulk_rename++ -c upper -v [path1]\n"
          << "  bulk_rename++ -d 2 -c upper -v [path1]\n"
          << "  bulk_rename++ -fi -c lower -v [path1]\n"
          << "  bulk_rename++ -ce noext -v [path1]\n"
          << "  bulk_rename++ -sym -c lower -v [path1]\n"
          << "  bulk_rename++ -sym -fi -c title -v [path1]\n"
          << "\x1B[0m\n";
}

// Extension stuff

// Global static transformations
static const std::vector<std::string> transformation_commands = {
    "lower",      // Convert to lowercase
    "upper",      // Convert to uppercase
    "reverse",    // Reverse the string
    "title",      // Convert to title case
    "snake",      // Convert to snake_case
    "rsnake",     // Convert to reverse snake_case
    "rspecial",   // Reverse special characters
    "rnumeric",   // Reverse numeric characters
    "rbra",       // Reverse brackets
    "roperand",   // Reverse operands
    "camel",      // Convert to camelCase
    "rcamel",     // Convert to reverse camelCase
    "kebab",      // Convert to kebab-case
    "rkebab",     // Convert to reverse kebab-case
    "nsequence",  // Normalize sequence numbers
    "rnsequence", // Reverse and normalize sequence numbers
    "date",       // Format date
    "rdate",      // Reverse date format
    "swap",       // Swap case
    "swapr",      // Reverse swap case
    "sentence",   // Convert to sentence case
    "pascal",     // Convert to PascalCase
    "rpascal",    // Convert to reverse PascalCase
    "bak",        // Backup
    "rbak",       // Reverse backup
    "noext"       // Remove extension
};


// Function to rename file extensions
void rename_extension(const std::vector<fs::path>& item_paths, const std::string& case_input, bool verbose_enabled, int& files_count, size_t batch_size, bool symlinks) {
    // Vector to store pairs of old and new paths for renaming
    std::vector<std::pair<fs::path, fs::path>> rename_batch;
    rename_batch.reserve(item_paths.size()); // Reserve space for efficiency

	// Iterate through each item path
	for (const auto& item_path : item_paths) {
		// Check if the item is a directory or a symlink
		if (fs::is_symlink(item_path) && !symlinks) {
			// Skip if it's a directory or symlink, print a message if verbose mode enabled
			if (verbose_enabled) {
                print_verbose_enabled("\033[0m\033[93mSkipped\033[0m \033[95msymlink_file\033[0m " + item_path.string() + " (excluded)", std::cout);
            
			}
			continue;
		}

        // Get the current extension of the file
        std::string extension = item_path.extension().string();
        std::string new_extension = extension;

        // Check if the requested case transformation is valid and apply it
        if (std::find(transformation_commands.begin(), transformation_commands.end(), case_input) != transformation_commands.end()) {
            // Case input is valid, perform appropriate transformation
            if (case_input == "lower") {
                std::transform(extension.begin(), extension.end(), new_extension.begin(), ::tolower);
            } else if (case_input == "upper") {
                std::transform(extension.begin(), extension.end(), new_extension.begin(), ::toupper);
            } else if (case_input == "reverse") {
                std::transform(extension.begin(), extension.end(), new_extension.begin(), [](char c) {
                    return std::islower(c) ? std::toupper(c) : std::tolower(c);
                });
            } else if (case_input == "title") {
                new_extension = capitalizeFirstLetter(new_extension);
            } else if (case_input == "bak") {
                if (extension.length() < 4 || extension.substr(extension.length() - 4) != ".bak") {
                    new_extension = extension + ".bak";
                } else {
                    new_extension = extension; // Keep the extension unchanged
                }
            } else if (case_input == "rbak") {
                if (extension.length() >= 4 && extension.substr(extension.length() - 4) == ".bak") {
                    new_extension = extension.substr(0, extension.length() - 4);
                }
            } else if (case_input == "noext") {
                new_extension.clear(); // Clearing extension removes it
            } else if (case_input == "swap") {
                new_extension = swapr_transform(extension);
            } else if (case_input == "swapr") {
                new_extension = swap_transform(extension);
            }

            // If extension changed, create new path and add to rename batch
            if (extension != new_extension) {
                fs::path new_path = item_path.parent_path() / (item_path.stem().string() + new_extension);
                rename_batch.emplace_back(item_path, new_path); // Add to the batch
            } else {
                // Print a message for skipped file if extension remains unchanged and parent directory is not a symlink
			if (verbose_enabled && !fs::is_symlink(item_path.parent_path()) && !symlinks) {
				print_verbose_enabled("\033[0m\033[93mSkipped\033[0m file " + item_path.string() + (extension.empty() ? " (no extension)" : " (extension unchanged)"), std::cout);
				} else if (verbose_enabled) {
					print_verbose_enabled("\033[0m\033[93mSkipped\033[0m file " + item_path.string() + (extension.empty() ? " (no extension)" : " (extension unchanged)"), std::cout);
				}
					
            }
        }

        // Batch processing: if batch size reached, rename batch and clear
        if (rename_batch.size() >= batch_size) {
            std::lock_guard<std::mutex> lock(files_mutex);
            batch_rename_extension(rename_batch, verbose_enabled, files_count);
            rename_batch.clear(); // Clear the batch after processing
        } else {
            // Process remaining items in the batch if any
            if (!rename_batch.empty()) {
                std::lock_guard<std::mutex> lock(files_mutex);
                batch_rename_extension(rename_batch, verbose_enabled, files_count);
            }
        }
    }
}


// Function to rename a batch of file extensions using multiple threads for parallel execution
void batch_rename_extension(const std::vector<std::pair<fs::path, fs::path>>& data, bool verbose_enabled, int& files_count) {
    // Determine the maximum available cores
    size_t max_cores = std::thread::hardware_concurrency();
    size_t num_threads = std::min(max_cores != 0 ? max_cores : 1, data.size());

    // Calculate the number of items per thread
    size_t chunk_size = data.size() / num_threads;

    // Use async tasks to perform renaming in parallel
    std::vector<std::future<void>> futures;
    for (size_t i = 0; i < num_threads; ++i) {
        auto future = std::async(std::launch::async, [=, &files_count]() {
            size_t start = i * chunk_size;
            size_t end = (i == num_threads - 1) ? data.size() : start + chunk_size;

            for (size_t j = start; j < end; ++j) {
                const auto& [old_path, new_path] = data[j];
                try {
                    // Attempt to rename the file
                    fs::rename(old_path, new_path);
                    {
                        // Safely increment files_count when a file is successfully renamed
                        std::lock_guard<std::mutex> lock(files_count_mutex);
                        ++files_count;
                    }
                    // Print a success message if verbose mode enabled
                    if (verbose_enabled) {
                        std::cout << "\033[0m\033[92mRenamed\033[0m file " << old_path.string() << " to " << new_path.string() << std::endl;
                    }
                } catch (const fs::filesystem_error& e) {
					if (verbose_enabled) {
                    // Print an error message if renaming fails
                    std::cerr << "\033[1;91mError\033[0m: " << e.what() << std::endl;
					}
                }
            }
        });
        futures.push_back(std::move(future));
    }

    // Wait for all async tasks to finish
    for (auto& future : futures) {
        future.wait();
    }
}


// Function to search subdirs for file extensions recursively for multiple paths in parallel
void rename_extension_path(const std::vector<std::string>& paths, const std::string& case_input, bool verbose_enabled, int depth, int& files_count, bool symlinks) {
    // If depth is negative, set it to a very large number to effectively disable the depth limit
    if (depth < 0) {
        depth = std::numeric_limits<int>::max();
    }

    auto start_time = std::chrono::steady_clock::now(); // Start time measurement

    // Get the maximum number of threads supported by the system
    unsigned int max_threads = std::thread::hardware_concurrency();
    if (max_threads == 0) {
        max_threads = 1; // Default to 1 thread if hardware concurrency is not available
    }

    // Determine the number of threads to create (minimum of max_threads and paths.size())
    unsigned int num_threads = std::min(max_threads, static_cast<unsigned int>(paths.size()));

    // Vector to store futures
    std::vector<std::future<void>> futures;

    // Calculate batch size
    int batch_size = paths.size() / num_threads;

    // Define the function to process each subset of paths asynchronously
    auto process_paths_async = [&case_input, verbose_enabled, depth, &files_count, batch_size, symlinks](const std::vector<std::string>& paths_subset) {
    for (const auto& path : paths_subset) {
        std::queue<std::pair<std::string, int>> directories; // Queue to store directories and their depths
        directories.push({path, 0}); // Push the initial path onto the queue with depth 0

        std::string depth_limit_reached_path; // Store the path where depth limit is reached

        while (!directories.empty()) {
            auto [current_path, current_depth] = directories.front();
            directories.pop();

            // Check if depth limit is reached
            if (current_depth >= depth && depth_limit_reached_path.empty()) {
                depth_limit_reached_path = current_path; // Store the path where depth limit is reached
                continue; // Skip processing this directory
            }

            fs::path current_fs_path(current_path);

            // Check if path exists
            if (verbose_enabled && !fs::exists(current_fs_path)) {
                print_error("\033[1;91mError: path does not exist - " + current_path + "\033[0m\n");
                continue;
            }

            // Process directories and files
            if (fs::is_directory(current_fs_path)) {
                for (const auto& entry : fs::directory_iterator(current_fs_path)) {
                    if (fs::is_symlink(entry)) {
                        if (!symlinks && verbose_enabled) {
                            // Print message for symlinked folder or file if symlinks flag is false
                            if (fs::is_directory(entry)) {
                                std::cout << "\033[0m\033[93mSkipped\033[0m processing \033[95msymlink_folder\033[0m " << entry.path().string() << " (excluded)\n";
                            } else if (!fs::is_directory(entry)) {
                                std::cout << "\033[0m\033[93mSkipped\033[0m \033[95msymlink_file\033[0m " << entry.path().string() << " (excluded)\n";
                            }
                        } else if (symlinks) {
                            // Process symlink if symlinks flag is true
                            directories.push({entry.path().string(), current_depth + 1}); // Push symlink as regular directory
                        }
                    } else if (fs::is_directory(entry)) {
                        directories.push({entry.path().string(), current_depth + 1}); // Push subdirectories onto the queue with incremented depth
                    } else if (fs::is_regular_file(entry)) {
                        rename_extension({entry.path()}, case_input, verbose_enabled, files_count, batch_size, symlinks);
                    }
                }
            } else if (fs::is_regular_file(current_fs_path)) {
                rename_extension({current_fs_path}, case_input, verbose_enabled, files_count, batch_size, symlinks);
            } else {
				if (verbose_enabled) {
                print_error("\033[1;91mError: specified path is neither a directory nor a regular file\033[0m\n");
				}
            }
        }
    }
};

    // Launch asynchronous tasks for each subset of paths
    for (unsigned int i = 0; i < num_threads; ++i) {
        auto future = std::async(std::launch::async, process_paths_async, std::vector<std::string>(paths.begin() + i * batch_size, paths.begin() + (i + 1) * batch_size));
        futures.push_back(std::move(future));
    }

    // Wait for all asynchronous tasks to finish
    for (auto& future : futures) {
        future.wait();
    }

    auto end_time = std::chrono::steady_clock::now(); // End time measurement

    // Calculate elapsed time
    std::chrono::duration<double> elapsed_seconds = end_time - start_time;

    // Print summary
    std::cout << "\n\033[1mRenamed \033[4mfile extensions\033[0m\033[1m to \033[1;38;5;214m" << case_input << "_case\033[0m\033[1m: for \033[1;92m" << files_count << " file(s) \033[0m\033[1mfrom \033[1;95m" << paths.size()
              << " input path(s) \033[0m\033[1min " << std::setprecision(1)
              << std::fixed << elapsed_seconds.count() << "\033[1m second(s)\n";
}


// Rename file&directory stuff
 
// Function to rename files
void rename_file(const fs::path& item_path, const std::string& case_input, bool is_directory, bool verbose_enabled, bool transform_dirs, bool transform_files, int& files_count, int& dirs_count, size_t batch_size, bool symlinks ) {
    
    // Check if the item is a symbolic link
    if (!fs::is_regular_file(item_path) || (fs::is_symlink(item_path) && !symlinks)) {
        if (verbose_enabled && transform_files && !symlinks) {
            print_verbose_enabled("\033[0m\033[93mSkipped\033[0m \033[95msymlink_file\033[0m " + item_path.string() + " (excluded)", std::cout);
        }
        return; // Skip processing symbolic links
    }
    
    std::vector<std::pair<fs::path, std::string>> rename_data;

    // Check if the item is a directory
    if (is_directory) {
        // If it is a directory, recursively process its contents
        for (const auto& entry : fs::directory_iterator(item_path)) {
            if (entry.is_directory()) {
                // Recursively call rename_file for subdirectories
                rename_file(entry.path(), case_input, true, verbose_enabled, transform_dirs, transform_files, files_count, dirs_count, batch_size, symlinks);
            } else {
                // Check if it's a symbolic link
                if (fs::is_symlink(entry.path()) && symlinks) {
                    // If it's a symbolic link, process it based on transform_dirs
                    if (transform_dirs && symlinks) {
                        // Process symbolic link files within regular directories
                        rename_file(entry.path(), case_input, false, verbose_enabled, transform_dirs, transform_files, files_count, dirs_count, batch_size, symlinks);
                    }
                } else {
                    // Process regular files within regular directories
                    rename_file(entry.path(), case_input, false, verbose_enabled, transform_dirs, transform_files, files_count, dirs_count, batch_size, symlinks);
                }
            }
        }
        // Increment the directory count
        std::lock_guard<std::mutex> lock(dirs_count_mutex);
        ++dirs_count;
        return;
    }

    // Extract the relative path of the item from its parent directory
    fs::path relative_path = item_path.filename();
    fs::path parent_path = item_path.parent_path();

    // Initialize variables for new name and path
    std::string name = relative_path.string();
    std::string new_name = name;
    fs::path new_path;

    // Perform transformations on file names if requested
    if (transform_files) {
        for (const auto& transformation : transformation_commands) {
            if (case_input.find(transformation) != std::string::npos) {
                // Apply the corresponding transformation
                if (transformation == "lower") {
                    std::transform(new_name.begin(), new_name.end(), new_name.begin(), ::tolower);
                } else if (transformation == "upper") {
                    std::transform(new_name.begin(), new_name.end(), new_name.begin(), ::toupper);
                } else if (transformation == "reverse") {
                    std::transform(new_name.begin(), new_name.end(), new_name.begin(), [](unsigned char c) {
                        return std::islower(c) ? std::toupper(c) : std::tolower(c);
                    });
                } else if (transformation == "title") {
                    new_name = capitalizeFirstLetter(new_name);
                } else if (transformation == "snake") {
                    std::replace(new_name.begin(), new_name.end(), ' ', '_');
                } else if (transformation == "rsnake") {
                    std::replace(new_name.begin(), new_name.end(), '_', ' ');
                } else if (transformation == "kebab") {
                    std::replace(new_name.begin(), new_name.end(), ' ', '-');
                } else if (transformation == "rkebab") {
                    std::replace(new_name.begin(), new_name.end(), '-', ' ');
                } else if (transformation == "rspecial") {
                    // Remove special characters from the name
                    new_name.erase(std::remove_if(new_name.begin(), new_name.end(), [](char c) {
                        return !std::isalnum(c) && c != '.' && c != '_' && c != '-' && c != '(' && c != ')' && c != '[' && c != ']' && c != '{' && c != '}' && c != '+' && c != '*' && c != '<' && c != '>' && c != ' '; // Retain
                    }), new_name.end());
                } else if (transformation == "rnumeric") {
                    // Remove numeric characters from the name
                    new_name.erase(std::remove_if(new_name.begin(), new_name.end(), [](char c) {
                        return std::isdigit(c);
                    }), new_name.end());
                } else if (transformation == "rbra") {
                    // Remove [ ] { } from the name
                    new_name.erase(std::remove_if(new_name.begin(), new_name.end(), [](char c) {
                        return c == '[' || c == ']' || c == '{' || c == '}' || c == '(' || c == ')';
                    }), new_name.end());
                } else if (transformation == "roperand") {
                    // Remove - + > < = * from the name
                    new_name.erase(std::remove_if(new_name.begin(), new_name.end(), [](char c) {
                        return c == '-' || c == '+' || c == '>' || c == '<' || c == '=' || c == '*';
                    }), new_name.end());
                } else if (transformation == "camel") {
                    new_name = to_camel_case(new_name);
                } else if (transformation == "rcamel") {
                    new_name = from_camel_case(new_name);
                } else if (transformation == "nsequence") {
                    // Check if the filename is already numbered
                    new_name = append_numbered_prefix(parent_path, new_name);
                } else if (transformation == "rnsequence") {
                    new_name = remove_numbered_prefix(new_name);
                } else if (transformation == "date") {
                    new_name = append_date_seq(new_name);
                } else if (transformation == "rdate") {
                    new_name = remove_date_seq(new_name);
                } else if (transformation == "sentence") {
                    new_name = sentenceCase(new_name);
                } else if (transformation == "swap") {
                    new_name = swap_transform(new_name);
                } else if (transformation == "swapr") {
                    new_name = swapr_transform(new_name);
                } else if (transformation == "pascal") {
                    new_name = to_pascal(new_name);
                } else if (transformation == "rpascal") {
                    new_name = from_pascal_case(new_name);
                }
            }
        }
    }

    // Add data to the list if new name differs
    if (name != new_name) {
        std::lock_guard<std::mutex> lock(files_mutex);
        rename_data.push_back({item_path, new_name});
    }

    // Check if batch size is reached and perform renaming
    if (rename_data.size() >= batch_size) {
        std::lock_guard<std::mutex> lock(files_mutex);
        rename_batch(rename_data, verbose_enabled, files_count, dirs_count);
        rename_data.clear();
    } else {
        // Rename any remaining data after processing the loop
        if (!rename_data.empty()) {
            std::lock_guard<std::mutex> lock(files_mutex);
            rename_batch(rename_data, verbose_enabled, files_count, dirs_count);
        }
        // Verbose output for skipped files with unchanged names
        if (name == new_name && verbose_enabled && transform_files && !fs::is_symlink(parent_path) && !symlinks) {
            print_verbose_enabled("\033[0m\033[93mSkipped\033[0m file " + item_path.string() + (name.empty() ? " (no name change)" : " (name unchanged)"), std::cout);
        } else if (name == new_name && verbose_enabled && transform_files && symlinks) {
            print_verbose_enabled("\033[0m\033[93mSkipped\033[0m file " + item_path.string() + (name.empty() ? " (no name change)" : " (name unchanged)"), std::cout);
        }
    }
}


// Function to rename a batch of files using multiple threads for parallel execution
void rename_batch(const std::vector<std::pair<fs::path, std::string>>& data, bool verbose_enabled, int& files_count, int& dirs_count) {
    // Determine the maximum available cores
    size_t max_cores = std::thread::hardware_concurrency();
    size_t num_threads = std::min(max_cores != 0 ? max_cores : 1, data.size());

    // Calculate the number of iterations per thread
    size_t chunk_size = data.size() / num_threads;

    // Use async tasks to perform renaming in parallel
    std::vector<std::future<void>> futures;
    for (size_t i = 0; i < num_threads; ++i) {
        auto future = std::async(std::launch::async, [=, &files_count, &dirs_count]() {
            size_t start = i * chunk_size;
            size_t end = (i == num_threads - 1) ? data.size() : start + chunk_size;

            for (size_t j = start; j < end; ++j) {
                const auto& [item_path, new_name] = data[j];
                fs::path new_path = item_path.parent_path() / new_name;
                try {
                    // Attempt to rename the file/directory
                    fs::rename(item_path, new_path);
                    if (verbose_enabled) {
                        // Print a success message if verbose mode enabled
                        print_verbose_enabled("\033[0m\033[92mRenamed\033[0m file " + item_path.string() + " to " + new_path.string(), std::cout);
                    }
                    // Update files_count or dirs_count based on the type of the renamed item
                    std::filesystem::directory_entry entry(new_path);
                    if (entry.is_regular_file()) {
                        // Update files_count when a file is successfully renamed
                        std::lock_guard<std::mutex> lock(files_count_mutex);
                        ++files_count;
                    } else {
                        // Update dirs_count when a directory is successfully renamed
                        std::lock_guard<std::mutex> lock(dirs_count_mutex);
                        ++dirs_count;
                    }
                } catch (const fs::filesystem_error& e) {
					if (verbose_enabled) {
                    // Print an error message if renaming fails
                    print_error("\033[1;91mError\033[0m: " + std::string(e.what()) + "\n", std::cerr);
					}
                }
            }
        });
        futures.push_back(std::move(future));
    }

    // Wait for all async tasks to finish
    for (auto& future : futures) {
        future.wait();
    }
}


// Function to process a directory entry (file or directory) based on specified transformations
void process_forParents(const fs::directory_entry& entry, const std::string& case_input, bool verbose_enabled, bool transform_dirs, bool transform_files, int& files_count, int& dirs_count, int depth, bool symlinks) {
    if (entry.is_directory()) {
        // If the entry is a directory, rename it accordingly
        rename_directory(entry.path(), case_input, false, verbose_enabled, transform_dirs, transform_files, files_count, dirs_count, depth, symlinks);
    } else {
        // If the entry is a file, rename it accordingly
        rename_file(entry.path(), case_input, false, verbose_enabled, transform_dirs, transform_files, files_count, dirs_count, batch_size, symlinks);
    }
}


// Function to rename a directory based on specified transformations
void rename_directory(const fs::path& directory_path, const std::string& case_input, bool rename_parents, bool verbose_enabled, bool transform_dirs, bool transform_files, int& files_count, int& dirs_count, int depth, bool symlinks) {
    std::string dirname = directory_path.filename().string();
    std::string new_dirname = dirname; // Initialize with the original name
    bool renaming_message_printed = false;
    

    // Determine the maximum number of threads supported by the system
    unsigned int num_threads = 1;
    unsigned int max_threads = std::thread::hardware_concurrency();
    if (max_threads == 0) {
        max_threads = 1; // If hardware concurrency is not available, default to 1 thread
    }
    

    // Early exit if the directory is a symlink
    if (fs::is_symlink(directory_path) && transform_dirs && !symlinks) {
        if (verbose_enabled) {
            // Print a message if verbose mode enabled
            print_verbose_enabled("\033[0m\033[93mSkipped\033[0m processing \033[95msymlink_folder\033[0m " + directory_path.string() + " (excluded)");
        }
        return;
    }
    
    // Early exit if the directory is a symlink
    if (fs::is_symlink(directory_path) && !transform_dirs && !symlinks) {
        if (verbose_enabled) {
            // Print a message if verbose mode enabled
            print_verbose_enabled("\033[0m\033[93mSkipped\033[0m processing \033[95msymlink_folder\033[0m " + directory_path.string() + " (excluded)");
        }
        return;
    }

    // Apply transformations to the directory name if required
    if (transform_dirs) {
        for (const auto& transformation : transformation_commands) {
            if (case_input == transformation) {
                // Apply the corresponding transformation
                if (transformation == "lower") {
                    std::transform(new_dirname.begin(), new_dirname.end(), new_dirname.begin(), ::tolower);
                } else if (transformation == "upper") {
                    std::transform(new_dirname.begin(), new_dirname.end(), new_dirname.begin(), ::toupper);
                } else if (transformation == "reverse") {
                    std::transform(new_dirname.begin(), new_dirname.end(), new_dirname.begin(), [](unsigned char c) {
                        return std::islower(c) ? std::toupper(c) : std::tolower(c);
                    });
                } else if (case_input == "title") {
                    new_dirname = capitalizeFirstLetter(new_dirname);
                } else if (transformation == "snake") {
                    std::replace(new_dirname.begin(), new_dirname.end(), ' ', '_');
                } else if (transformation == "rsnake") {
                    std::replace(new_dirname.begin(), new_dirname.end(), '_', ' ');
                } else if (transformation == "kebab") {
                    std::replace(new_dirname.begin(), new_dirname.end(), ' ', '-');
                } else if (transformation == "rkebab") {
                    std::replace(new_dirname.begin(), new_dirname.end(), '-', ' ');
                } else if (transformation == "rspecial") {
                    // Remove special characters from the directory name
                    new_dirname.erase(std::remove_if(new_dirname.begin(), new_dirname.end(), [](char c) {
                        return !std::isalnum(c) && c != '.' && c != '_' && c != '-' && c != '(' && c != ')' && c != '[' && c != ']' && c != '{' && c != '}' && c != '+' && c != '*' && c != '<' && c != '>' && c != ' ';
                    }), new_dirname.end());
                } else if (transformation == "rnumeric") {
                    // Remove numeric characters from the directory name
                    new_dirname.erase(std::remove_if(new_dirname.begin(), new_dirname.end(), [](char c) {
                        return std::isdigit(c);
                    }), new_dirname.end());
                } else if (transformation == "rbra") {
                    // Remove [ ] { } from the directory name
                    new_dirname.erase(std::remove_if(new_dirname.begin(), new_dirname.end(), [](char c) {
                        return c == '[' || c == ']' || c == '{' || c == '}' || c == '(' || c == ')';
                    }), new_dirname.end());
                } else if (transformation == "roperand") {
                    // Remove - + > < = * from the directory name
                    new_dirname.erase(std::remove_if(new_dirname.begin(), new_dirname.end(), [](char c) {
                        return c == '-' || c == '+' || c == '>' || c == '<' || c == '=' || c == '*';
                    }), new_dirname.end());
                } else if (transformation == "camel") {
                    new_dirname = to_camel_case(new_dirname);
                } else if (transformation == "rcamel") {
                    new_dirname = from_camel_case(new_dirname);
                } else if (transformation == "swap") {
                    new_dirname = swap_transform(new_dirname);
                } else if (transformation == "swapr") {
                    new_dirname = swapr_transform(new_dirname);
                } else if (transformation == "nsequence") {
                    rename_folders_with_sequential_numbering(directory_path, dirs_count, verbose_enabled, symlinks);
                } else if (transformation == "rnsequence") {
                    remove_sequential_numbering_from_folders(directory_path, dirs_count, verbose_enabled, symlinks);
                } else if (transformation == "date") {
                    rename_folders_with_date_suffix(directory_path, dirs_count, verbose_enabled, symlinks);
                } else if (transformation == "rdate") {
                    remove_date_suffix_from_folders(directory_path, dirs_count, verbose_enabled, symlinks);
                } else if (transformation == "sentence") {
                    new_dirname = sentenceCase(new_dirname);
                } else if (transformation == "pascal") {
                    new_dirname = to_pascal(new_dirname);
                } else if (transformation == "rpascal") {
                    new_dirname = from_pascal_case(new_dirname);
                }
                break;
            }
        }
    }

    fs::path new_path = directory_path.parent_path() / std::move(new_dirname); // Move new_dirname instead of copying

    // Check if renaming is necessary
    if (directory_path != new_path) {
        try {
            // Attempt to rename the directory
            fs::rename(directory_path, new_path);

            if (verbose_enabled && !renaming_message_printed) {
                // Print a renaming message if verbose mode enabled
                print_verbose_enabled("\033[0m\033[92mRenamed\033[0m\033[94m directory\033[0m " + directory_path.string() + " to " + new_path.string());
                renaming_message_printed = true; // Set the flag to true after printing the message
            }
            std::lock_guard<std::mutex> lock(dirs_count_mutex);
            ++dirs_count; // Increment the directory count
        } catch (const fs::filesystem_error& e) {
            if (e.code() == std::errc::permission_denied) {
				if (verbose_enabled) {
            // Handle permission denied error
            print_error("\033[1;91mError\033[0m: Permission denied: " + directory_path.string());
				}
			}
			return; // Exit early if permission errors found
        }
    } else {
        // If the directory name remains unchanged
        if (verbose_enabled && !transform_files && !special) {
            // Print a message indicating that the directory was skipped (no name change)
            print_verbose_enabled("\033[0m\033[93mSkipped\033[0m\033[94m directory\033[0m " + directory_path.string() + " (name unchanged)");
        } else if (verbose_enabled && transform_dirs && transform_files && !special) {
            // Print a message indicating that the directory was skipped (name unchanged)
            print_verbose_enabled("\033[0m\033[93mSkipped\033[0m\033[94m directory\033[0m " + directory_path.string() + " (name unchanged)");
        }
    }

    // Continue recursion if the depth limit is not reached
    if (depth != 0) {
        // Decrement depth only if the depth limit is positive
        if (depth > 0)
            --depth;

        // Determine the number of threads to use for processing subdirectories
        num_threads = std::min(max_threads, num_threads);

        // Vector to store futures for asynchronous tasks
        std::vector<std::future<void>> futures;

        // Iterate over subdirectories of the renamed directory
        for (const auto& entry : fs::directory_iterator(new_path)) {
            if (entry.is_directory()) {
                if (rename_parents && num_threads <= max_threads) {
                    // Process subdirectories asynchronously if renaming parents is enabled
                    futures.push_back(std::async(std::launch::async, process_forParents, entry, case_input, verbose_enabled, transform_dirs, transform_files, std::ref(files_count), std::ref(dirs_count), depth, symlinks));
                } else if (futures.size() < num_threads) {
                    // Process subdirectories asynchronously if max_threads is not reached
                    futures.push_back(std::async(std::launch::async, rename_directory, entry.path(), case_input, false, verbose_enabled, transform_dirs, transform_files, std::ref(files_count), std::ref(dirs_count), depth, symlinks));
                } else {
                    // Process subdirectories in the main thread if max_threads is reached
                    rename_directory(entry.path(), case_input, false, verbose_enabled, transform_dirs, transform_files, files_count, dirs_count, depth, symlinks);
                }
            } else {
                // Process files in the main thread
                rename_file(entry.path(), case_input, false, verbose_enabled, transform_dirs, transform_files, files_count, dirs_count, batch_size, symlinks);
            }
        }

        // Wait for all asynchronous tasks to finish
        for (auto& future : futures) {
            future.get();
        }
    }
}


// Function to rename paths (directories and files) based on specified transformations
void rename_path(const std::vector<std::string>& paths, const std::string& case_input, bool rename_parents, bool verbose_enabled, bool transform_dirs, bool transform_files, int depth, bool symlinks) {
    // Check if case_input is empty
    if (case_input.empty()) {
        print_error("\033[1;91mError: Case conversion mode not specified (-c option is required)\n\033[0m");
        return;
    }

    // Determine the maximum number of threads supported by the system
    unsigned int max_threads = std::thread::hardware_concurrency();
    if (max_threads == 0) {
        max_threads = 1; // If hardware concurrency is not available, default to 1 thread
    }

    auto start_time = std::chrono::steady_clock::now(); // Start time measurement

    int files_count = 0; // Initialize files count
    int dirs_count = 0;  // Initialize directories count

    // Calculate num_threads as the minimum of max_threads and the number of paths
    unsigned int num_threads = std::min(max_threads, static_cast<unsigned int>(paths.size()));

    std::vector<std::future<void>> futures;

    // Process paths asynchronously
    for (unsigned int i = 0; i < num_threads; ++i) {
        futures.push_back(std::async(std::launch::async, [&paths, i, &case_input, rename_parents, verbose_enabled, transform_dirs, transform_files, depth, symlinks, &files_count, &dirs_count]() {
            // Obtain the current path
            fs::path current_path(paths[i]);

            if (fs::exists(current_path)) {
                if (fs::is_directory(current_path)) {
                    if (rename_parents) {
                        // If -p option is used, only rename the immediate parent
                        fs::path immediate_parent_path = current_path.parent_path();
                        rename_directory(immediate_parent_path, case_input, rename_parents, verbose_enabled, transform_dirs, transform_files, files_count, dirs_count, depth, symlinks);
                    } else {
                        // Otherwise, rename the entire path
                        rename_directory(current_path, case_input, rename_parents, verbose_enabled, transform_dirs, transform_files, files_count, dirs_count, depth, symlinks);
                    }
                } else if (fs::is_regular_file(current_path)) {
                    // For files, directly rename the item without considering the parent directory
                    rename_file(current_path, case_input, false, verbose_enabled, transform_dirs, transform_files, files_count, dirs_count, batch_size, symlinks);
                } else {
					if (verbose_enabled) {
                    print_error("\033[1;91mError: specified path is neither a directory nor a regular file\033[0m\n");
					
                }
            } }else {
				if (verbose_enabled) {
                print_error("\033[1;91mError: path does not exist - " + paths[i] + "\033[0m\n");
				}
            }
        }));
    }

    // Wait for all asynchronous tasks to finish
    for (auto& future : futures) {
        future.wait();
    }

    // Process remaining paths in the main thread
    for (unsigned int i = num_threads; i < paths.size(); ++i) {
        fs::path current_path(paths[i]);

        if (fs::exists(current_path)) {
            if (fs::is_directory(current_path)) {
                if (rename_parents) {
                    // If -p option is used, only rename the immediate parent
                    fs::path immediate_parent_path = current_path.parent_path();
                    rename_directory(immediate_parent_path, case_input, rename_parents, verbose_enabled, transform_dirs, transform_files, files_count, dirs_count, depth, symlinks);
                } else {
                    // Otherwise, rename the entire path in the main thread
                    rename_directory(current_path, case_input, rename_parents, verbose_enabled, transform_dirs, transform_files, files_count, dirs_count, depth, symlinks);
                }
            } else if (fs::is_regular_file(current_path)) {
                // For files, directly rename the item without considering the parent directory
                rename_file(current_path, case_input, false, verbose_enabled, transform_dirs, transform_files, files_count, dirs_count, batch_size, symlinks);
            } else {
				if (verbose_enabled) {
                print_error("\033[1;91mError: specified path is neither a directory nor a regular file\033[0m\n");
				}
            }
        } else {
           // print_error("\033[1;91mError: path does not exist - " + paths[i] + "\033[0m\n");
        }
    }

    auto end_time = std::chrono::steady_clock::now(); // End time measurement

    std::chrono::duration<double> elapsed_seconds = end_time - start_time; // Calculate elapsed time

    // Output summary of the renaming process
    std::cout << "\n\033[0m\033[1mRenamed to \033[1;38;5;214m" << case_input << "_case\033[0m\033[1m: \033[1;92m" << files_count << " file(s) \033[0m\033[1mand \033[1;94m"
              << dirs_count << " dir(s) \033[0m\033[1mfrom \033[1;95m" << paths.size()
              << " input path(s) \033[0m\033[1min " << std::setprecision(1)
              << std::fixed << elapsed_seconds.count() << "\033[1m second(s)\n";
}


int main(int argc, char *argv[]) {
    // Initialize variables and flags
    std::vector<std::string> paths;
    std::string case_input;
    bool rename_parents = false;
    bool rename_extensions = false;
    bool verbose_enabled = false;
    int files_count = 0;
    int depth = -1;
    bool case_specified = false;
    bool transform_dirs = true;
    bool transform_files = true;
    bool symlinks = false;

    // Handle command-line arguments
    // Display help message if no arguments provided
    if (argc == 1) {
        print_help();
        return 0;
    }
    
    // Check if --version flag is present
    if (argc > 1 && std::string(argv[1]) == "--version") {
        // Print version number and exit
        printVersionNumber("1.4.8");
        return 0;
    }

    // Flags to handle command-line options
    bool fi_flag = false;
    bool fo_flag = false;
    bool c_flag = false;
    bool cp_flag = false;
    bool ce_flag = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "-fi") {
            transform_dirs = false;
            fi_flag = true;
        } else if (arg == "-sym") {
            symlinks = true;
        } else if (arg == "-fo") {
            transform_files = false;
            fo_flag = true;
        } else if (arg == "-d" && i + 1 < argc) {
            // Check if the depth value is empty or not a number
            if (argv[i + 1] == nullptr || std::string(argv[i + 1]).empty() || !isdigit(argv[i + 1][0])) {
                print_error("\033[1;91mError: Depth value if set must be a non-negative integer.\033[0m\n");
                return 1;
            }
            depth = std::atoi(argv[++i]);
            if (depth < -1) {
                print_error("\033[1;91mError: Depth value if set must be -1 or greater.\033[0m\n");
                return 1;
            }
        } else if (arg == "-v" || arg == "--verbose") {
            verbose_enabled = true;
        } else if (arg == "-h" || arg == "--help") {
            std::system("clear");
            print_help();
            return 0;
        } else if (arg == "-c") {
            // Check if -c, -cp, -ce options are mixed
            if (c_flag || cp_flag || ce_flag) {
                print_error("\033[1;91mError: Cannot mix -c, -cp, and -ce options.\033[0m\n");
                return 1;
            }
            c_flag = true;
            if (i + 1 < argc) {
                case_input = argv[++i];
                case_specified = true;
            } else {
                print_error("\033[1;91mError: Missing argument for option " + arg + "\033[0m\n");
                return 1;
            }
        } else if (arg == "-cp") {
            // Check if -c, -cp, -ce options are mixed
            if (c_flag || cp_flag || ce_flag) {
                print_error("\033[1;91mError: Cannot mix -c, -cp, and -ce options.\033[0m\n");
                return 1;
            }
            cp_flag = true;
            rename_parents = true;
            if (i + 1 < argc) {
                case_input = argv[++i];
                case_specified = true;
            } else {
                print_error("\033[1;91mError: Missing argument for option " + arg + "\033[0m\n");
                return 1;
            }
        } else if (arg == "-ce") {
            // Check if -c, -cp, -ce options are mixed
            if (c_flag || cp_flag || ce_flag) {
                print_error("\033[1;91mError: Cannot mix -c, -cp, and -ce options.\033[0m\n");
                return 1;
            } else if (arg == "-ce") {
            // Check if -c, -cp, -ce options are mixed
            if (fi_flag || fo_flag || ce_flag) {
                print_error("\033[1;91mError: Cannot mix -fi or -fo with -ce option.\033[0m\n");
                return 1;
            }
		}
            
            ce_flag = true;
            rename_extensions = true;
            if (i + 1 < argc) {
                case_input = argv[++i];
                case_specified = true;
            } else {
                print_error("\033[1;91mError: Missing argument for option " + arg + "\033[0m\n");
                return 1;
            }
        } else {
            // Check for duplicate paths
            if (std::find(paths.begin(), paths.end(), arg) != paths.end()) {
                print_error("\033[1;91mError: Duplicate path detected - " + arg + "\033[0m\n");
                return 1;
            }
            paths.emplace_back(arg);
        }
    }

    // Perform renaming based on flags and options
    if (fi_flag && fo_flag) {
        print_error("\033[1;91mError: Cannot mix -fi and -fo options.\033[0m\n");
        return 1;
    }

    if (!case_specified) {
        print_error("\033[1;91mError: Case conversion mode not specified (-c, -cp, or -ce option is required)\033[0m\n");
        return 1;
    }

    // Check for valid case modes
    std::vector<std::string> valid_modes;
    if (cp_flag || c_flag) { // Valid modes for -cp and -ce
        valid_modes = {"lower", "upper", "reverse", "title", "date", "swap","swapr","rdate", "pascal", "rpascal", "camel","sentence", "rcamel", "kebab", "rkebab", "rsnake", "snake", "rnumeric", "rspecial", "rbra", "roperand", "nsequence", "rnsequence"};
    } else { // Valid modes for -c
        valid_modes = {"lower", "upper", "reverse", "title", "swap", "swapr", "rbak", "bak", "noext"};
    }

    if (std::find(valid_modes.begin(), valid_modes.end(), case_input) == valid_modes.end()) {
        print_error("\033[1;91mError: Unspecified or invalid case mode - " + case_input + ". Run 'bulk_rename++ --help'.\033[0m\n");
        return 1;
    }

    // Check if paths exist
    for (const auto& path : paths) {
        if (!fs::exists(path)) {
            print_error("\033[1;91mError: Path does not exist - " + path + "\033[0m\n");
            return 1;
        }
    }
    
    // Check if paths end with '/'
    for (const std::string& path : paths) {
        if (path.back() != '/') {
            print_error("\033[1;91mError: Path(s) must end with '/' - \033[0m\033[1me.g. \033[1;91m" + path + " \033[0m\033[1m-> \033[1;94m" + path +"/\033[0m" "\n\033[0m");
            return 0;
        }
    }

    std::system("clear");
    //if (case_input == "rnumeric" || case_input == "rspecial" || case_input == "rbra" || case_input == "roperand" || case_input == "noext") {
    //    std::cout << "\033[1;93m!!! WARNING SELECTED OPERATION IRREVERSIBLE !!!\033[0m\n\n";
    //}

	// Prompt the user for confirmation before proceeding
	std::string confirmation;
	if (rename_parents) {
		// Display the paths and their lowest parent directories that will be renamed
		std::cout << "\033[0m\033[1mThe following path(s) and the \033[4mlowest Parent\033[0m\033[1m dir(s), will be recursively renamed to \033[0m\e[1;38;5;214m" << case_input << "Case\033[0m";
		if (depth != -1) {
			std::cout << "\033[0m\033[1m (up to depth " << depth << ")";
		}
		if (!transform_dirs) {
			std::cout << "\033[0m\033[1m (excluding directories)";
		}
		if (!transform_files) {
			std::cout << "\033[0m\033[1m (excluding files)";
		}
		std::cout << ":\033[1m\n\n";
		for (const auto& path : paths) {
			std::cout << "\033[1;94m" << path << "\033[0m" << std::endl;
		}
	} else if (rename_extensions) {
		// Display the paths where file extensions will be recursively renamed
		std::cout << "\033[0m\033[1mThe file \033[4mextensions\033[0m\033[1m under the following path(s) \033[1mwill be recursively renamed to \033[0m\e[1;38;5;214m" << case_input << "Case\033[0m";
		if (depth != -1) {
			std::cout << "\033[0m\033[1m (up to depth " << depth << ")";
		}
		std::cout << ":\033[1m\n\n";
		for (const auto& path : paths) {
			std::cout << "\033[1;94m" << path << "\033[0m" << std::endl;
		}
	} else {
		// Display the paths that will be recursively renamed
		std::cout << "\033[0m\033[1mThe following path(s) will be recursively renamed to \033[0m\e[1;38;5;214m" << case_input << "Case\033[0m";
		if (depth != -1) {
			std::cout << "\033[0m\033[1m (up to depth " << depth << ")";
		}
		if (!transform_dirs && rename_parents) {
			std::cout << "\033[0m\033[1m (excluding both files and directories)";
		} else if (!transform_dirs) {
			std::cout << "\033[0m\033[1m (excluding directories)";
		} else if (!transform_files) {
			std::cout << "\033[0m\033[1m (excluding files)";
		}
		std::cout << ":\033[1m\n\n";
		for (const auto& path : paths) {
			std::cout << "\033[1;94m" << path << "\033[0m" << std::endl;
		}
	}

	// Prompt the user for confirmation
	std::cout << "\n\033[1mDo you want to proceed? (y/n): ";
	std::getline(std::cin, confirmation);

	// If verbose mode is enabled and the user confirms, output an empty line
	if (verbose_enabled && confirmation == "y") {
		std::cout << " " << std::endl;
	}

	// If the user does not confirm, abort the operation
	if (confirmation != "y") {
		std::cout << "\n\033[1;91mOperation aborted by user.\033[0m";
		std::cout << "\n" << std::endl;
		std::cout << "\033[1mPress enter to exit...";
		std::cin.get();
		std::system("clear");
		return 0;
	}

	// Perform the renaming operation based on the selected mode
	if (rename_parents) {
		rename_path(paths, case_input, true, verbose_enabled, transform_dirs, transform_files, depth, symlinks); // Pass true for rename_parents
	} else if (rename_extensions) {
		rename_extension_path(paths, case_input, verbose_enabled, depth, files_count, symlinks);
	} else {
		rename_path(paths, case_input, rename_parents, verbose_enabled, transform_dirs, transform_files, depth, symlinks);
	}

	// Prompt the user to press enter to exit
	std::cout << "\n\033[1mPress enter to exit...\033[0m";
	std::cin.get();
	std::system("clear");
	return 0;
}
