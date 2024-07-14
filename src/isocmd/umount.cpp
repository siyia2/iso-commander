#include "../headers.h"
#include "../threadpool.h"


// UMOUNT STUFF

// Function to list mounted ISOs in the /mnt directory
void listMountedISOs() {
    const char* isoPath = "/mnt";
    std::vector<std::string> isoDirs;
    isoDirs.reserve(100);  // Pre-allocate space for 100 entries

        DIR* dir = opendir(isoPath);
        if (dir == nullptr) {
            std::cerr << "\033[1;91mError opening the /mnt directory.\033[0;1m\n";
            return;
        }

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type == DT_DIR && strncmp(entry->d_name, "iso_", 4) == 0) {
                isoDirs.emplace_back(entry->d_name + 4);
            }
        }
        closedir(dir);

    if (isoDirs.empty()) {
        return;
    }

    sortFilesCaseInsensitive(isoDirs);

    std::ostringstream output;
    // Reserve estimated space for the string buffer
    std::string buffer;
    buffer.reserve(isoDirs.size() * 100);
    output.str(std::move(buffer));

    output << "\n";

    size_t numDigits = std::to_string(isoDirs.size()).length();

    for (size_t i = 0; i < isoDirs.size(); ++i) {
        const char* sequenceColor = (i % 2 == 0) ? "\033[31;1m" : "\033[32;1m";
        output << sequenceColor << std::setw(numDigits) << (i + 1) << ". "
               << "\033[94;1m/mnt/iso_\033[95;1m" << isoDirs[i] << "\033[0;1m\n";
    }

    std::cout << output.str();
}


// Function to check if directory is empty for unmountISO
bool isDirectoryEmpty(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return false;  // Unable to open directory
    }

    errno = 0;
    struct dirent* entry;
    int count = 0;
    while ((entry = readdir(dir)) != nullptr) {
        if (++count > 2) {
            closedir(dir);
            return false;  // Directory not empty
        }
    }

    closedir(dir);
    return errno == 0 && count <= 2;  // Empty if only "." and ".." entries and no errors
}


// Function to unmount ISO files asynchronously
void unmountISO(const std::vector<std::string>& isoDirs, std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors) {
    // Check for root privileges
    if (geteuid() != 0) {
        for (const auto& isoDir : isoDirs) {
            auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(isoDir);
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to unmount: \033[1;93m'" << isoDirectory << "/" << isoFilename 
                         << "\033[1;93m'\033[1;91m. Root privileges are required.\033[0m";
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                unmountedErrors.emplace(errorMessage.str());
            }
        }
        return;
    }

    struct libmnt_table *tb = mnt_new_table();
    struct libmnt_iter *itr = mnt_new_iter(MNT_ITER_BACKWARD);
    struct libmnt_fs *fs;
    std::vector<std::string> mountpoints_to_unmount;

    // Parse current mount table
    mnt_table_parse_mtab(tb, NULL);

    // Find mountpoints to unmount
    while (mnt_table_next_fs(tb, itr, &fs) == 0) {
        const char *target = mnt_fs_get_target(fs);
        if (target) {
            auto it = std::find(isoDirs.begin(), isoDirs.end(), target);
            if (it != isoDirs.end()) {
                mountpoints_to_unmount.push_back(target);
            }
        }
    }

    mnt_free_iter(itr);
    mnt_free_table(tb);

    std::vector<std::string> successfully_unmounted;

    // Unmount in batch
    struct libmnt_context *ctx = mnt_new_context();
    if (!ctx) {
        // Handle error: unable to create libmount context
        std::stringstream errorMessage;
        errorMessage << "\033[1;91mFailed to create libmount context.\033[0m";
        {
            std::lock_guard<std::mutex> lowLock(Mutex4Low);
            unmountedErrors.emplace(errorMessage.str());
        }
        return;
    }

    for (const auto& mp : mountpoints_to_unmount) {
        mnt_context_set_target(ctx, mp.c_str());
        mnt_context_set_mflags(ctx, MS_LAZYTIME);

        int rc = mnt_context_umount(ctx);
        if (rc != 0) {
            auto [isoDirectory, isoFilename] = extractDirectoryAndFilename(mp);
            std::stringstream errorMessage;
            if (!isDirectoryEmpty(mp)) {
                errorMessage << "\033[1;91mFailed to unmount: \033[1;93m'" << isoDirectory << "/" << isoFilename << "\033[1;93m'\033[1;91m. Probably not an ISO mountpoint.\033[0m";
                {
                    std::lock_guard<std::mutex> lowLock(Mutex4Low);
                    unmountedErrors.emplace(errorMessage.str());
                }
            }
        } else {
            successfully_unmounted.push_back(mp);
        }
    }

    mnt_free_context(ctx);

    // Remove empty directories in batch
    std::vector<std::string> dirs_to_remove;
    for (const auto& mp : successfully_unmounted) {
        if (isDirectoryEmpty(mp)) {
            dirs_to_remove.push_back(mp);
        }
    }

    for (const auto& dir : dirs_to_remove) {
        if (rmdir(dir.c_str()) == 0) {
            auto [directory, filename] = extractDirectoryAndFilename(dir);
            std::string removedDirInfo = "\033[0;1mUnmounted: \033[1;92m'" + directory + "/" + filename + "\033[1;92m'\033[0m.";
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                unmountedFiles.emplace(removedDirInfo);
            }
        } else {
            std::stringstream errorMessage;
            errorMessage << "\033[1;91mFailed to remove directory: \033[1;93m'" << dir << "'\033[1;91m.\033[0m";
            {
                std::lock_guard<std::mutex> lowLock(Mutex4Low);
                unmountedErrors.emplace(errorMessage.str());
            }
        }
    }
}


// Function to print unmounted ISOs and errors
void printUnmountedAndErrors(std::set<std::string>& unmountedFiles, std::set<std::string>& unmountedErrors, std::set<std::string>& errorMessages) {
	clearScrollBuffer();
	
    // Print unmounted files
    for (const auto& unmountedFile : unmountedFiles) {
        std::cout << "\n" << unmountedFile;
    }
    
    if (!unmountedErrors.empty() && !unmountedFiles.empty()) {
				std::cout << "\n";
			}
			
	for (const auto& unmountedError : unmountedErrors) {
        std::cout << "\n" << unmountedError;
    }
    
    if (!errorMessages.empty()) {
				std::cout << "\n";
			}
	unmountedFiles.clear();
	unmountedErrors.clear();
    // Print unique error messages
    for (const auto& errorMessage : errorMessages) {
            std::cerr << "\n\033[1;91m" << errorMessage << "\033[0m\033[1m";
        }
    errorMessages.clear();
}


// Main function for unmounting ISOs
void unmountISOs() {
    std::vector<std::string> isoDirs;
    std::set<std::string> errorMessages, unmountedFiles, unmountedErrors;
    const std::string isoPath = "/mnt";
    bool isFiltered = false;
    std::vector<std::string> filteredIsoDirs;
    filteredIsoDirs.reserve(100);

    while (true) {
        std::vector<std::string> selectedIsoDirs;
        selectedIsoDirs.reserve(maxThreads);
		
        clearScrollBuffer();
        listMountedISOs();
        if (!isFiltered) {
			isoDirs.clear();
		}
        unmountedFiles.clear();
        unmountedErrors.clear();
        errorMessages.clear();

        if (!isFiltered) {
            for (const auto& entry : std::filesystem::directory_iterator(isoPath)) {
                if (entry.is_directory() && entry.path().filename().string().find("iso_") == 0) {
                    isoDirs.push_back(entry.path().string());
                }
            }

            sortFilesCaseInsensitive(isoDirs);
        } else {
			clearScrollBuffer();
            sortFilesCaseInsensitive(filteredIsoDirs);
            std::cout << "\n";
            size_t maxIndex = filteredIsoDirs.size();
            size_t numDigits = std::to_string(maxIndex).length();

            for (size_t i = 0; i < filteredIsoDirs.size(); ++i) {
                std::string afterSlash = filteredIsoDirs[i].substr(filteredIsoDirs[i].find_last_of("/") + 1);
                std::string afterUnderscore = afterSlash.substr(afterSlash.find("_") + 1);
                std::string color = (i % 2 == 0) ? "\033[1;31m" : "\033[1;32m"; // Red if even, Green if odd

                std::cout << color << "\033[1m"
                          << std::setw(numDigits) << std::setfill(' ') << (i + 1) << ".\033[1;94m /mnt/iso_"
                          << "\033[1;95m" << afterUnderscore << "\033[0;1m\n";
            }
        }

        if (isoDirs.empty() && !isFiltered) {
            std::cerr << "\n\033[1;93mNo path(s) matching the '/mnt/iso_*' pattern found.\033[0m\033[1m\n";
            std::cout << "\n\033[1;32m↵ to continue...";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return;
        }

        std::string prompt = std::string(isFiltered ? "\n\001\033[1;96m\002Filtered \001\033[1;92m\002ISO" : "\n\001\033[1;92m\002ISO")
            + "\001\033[1;94m\002 ↵ for \001\033[1;93m\002umount\001\033[1;94m\002 (e.g., 1-3,1 5,00=all), / ↵ filter, ↵ return:\001\033[0;1m\002 ";

        std::unique_ptr<char[], decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
        std::string inputString(input.get());

        if (!inputString.empty() && inputString != "/") {
			clearScrollBuffer();
            std::cout << "\033[1m\n";
        }

        if (inputString.empty()) {
            if (isFiltered) {
                isFiltered = false;
                filteredIsoDirs.clear(); // Clear the filtered list when returning to unfiltered mode
                continue;
            } else {
                return;
            }
        } else if (inputString == "/") {
			std::vector<std::string> lastFilteredIsoDirs = filteredIsoDirs;  // Preserve last filtered results
			while (true) {    
				unmountedFiles.clear();
				unmountedErrors.clear();
				errorMessages.clear();
				clear_history();
				historyPattern = true;
				loadHistory();            
				std::string filterPrompt = "\001\033[1A\002\001\033[K\002\001\033[1A\002\001\033[K\002\n\001\033[38;5;94m\002FilterTerms\001\033[1;94m\002 ↵ for \001\033[1;93m\002umount\001\033[1;94m\002 list (multi-term separator: \001\033[1;93m\002;\001\033[1;94m\002), ↵ return: \001\033[0;1m\002";
				std::unique_ptr<char, decltype(&std::free)> searchQuery(readline(filterPrompt.c_str()), &std::free);
				std::string terms(searchQuery.get());
        
				if (!searchQuery || searchQuery.get()[0] == '\0' || strcmp(searchQuery.get(), "/") == 0) {
					historyPattern = false;
					clear_history();
					// Restore last filtered results or original list
					if (filteredIsoDirs.empty() && !lastFilteredIsoDirs.empty()) {
						filteredIsoDirs = lastFilteredIsoDirs;
						isFiltered = true;
					} else if (filteredIsoDirs.empty()) {
						filteredIsoDirs = isoDirs;
						isFiltered = false;
					}
					break;
				}
				std::string inputSearch(searchQuery.get());
				std::cout << "\033[1m\n";
				if (strcmp(searchQuery.get(), "/") != 0) {
					add_history(searchQuery.get());
					saveHistory();
				}
        
				historyPattern = false;
				clear_history();
    
				std::vector<std::string> filterPatterns;
				std::stringstream ss(terms);
				std::string token;
				while (std::getline(ss, token, ';')) {
					filterPatterns.push_back(token);
					toLowerInPlace(filterPatterns.back());
				}
    
				filteredIsoDirs.clear();
				size_t numDirs = isoDirs.size();
				unsigned int numThreads = std::min(static_cast<unsigned int>(numDirs), maxThreads);
				std::vector<std::future<void>> futuresFilter;
				futuresFilter.reserve(numThreads);
				size_t baseDirsPerThread = numDirs / numThreads;
				size_t remainder = numDirs % numThreads;
				for (size_t i = 0; i < numThreads; ++i) {
					size_t start = i * baseDirsPerThread + std::min(i, remainder);
					size_t end = start + baseDirsPerThread + (i < remainder ? 1 : 0);
					futuresFilter.push_back(std::async(std::launch::async, [&, start, end] {
						filterMountPoints(isoDirs, filterPatterns, filteredIsoDirs, start, end);
					}));
				}
				for (auto& future : futuresFilter) {
					future.get();
				}
    
				if (filteredIsoDirs.empty()) {
					std::cout << "\033[1A\033[K";
					// Restore previous filtered list if current filtering yields no results
					filteredIsoDirs = lastFilteredIsoDirs;
					continue;
				} else {
					clearScrollBuffer();
					lastFilteredIsoDirs = filteredIsoDirs;  // Update last filtered results
					isFiltered = true;
					break;
				}
			}
		} else {
			std::vector<std::string>& currentDirs = isFiltered ? filteredIsoDirs : isoDirs;

            if (inputString == "00") {
                selectedIsoDirs = currentDirs;
            } else {
                std::set<size_t> selectedIndices;
                std::istringstream iss(inputString);
                for (std::string token; iss >> token;) {
                    if (startsWithZero(token)) {
                        errorMessages.emplace("\033[1;91mInvalid index: '0'.\033[0;1m");
                        continue;
                    }
                    try {
                        size_t dashPos = token.find('-');
                        if (dashPos != std::string::npos) {
                            size_t start = std::stoi(token.substr(0, dashPos)) - 1;
                            size_t end = std::stoi(token.substr(dashPos + 1)) - 1;
                            if (start < currentDirs.size() && end < currentDirs.size()) {
                                for (size_t i = std::min(start, end); i <= std::max(start, end); ++i) {
                                    selectedIndices.emplace(i);
                                }
                            } else {
                                errorMessages.emplace("Invalid range: '" + token + "'.");
                            }
                        } else {
                            size_t index = std::stoi(token) - 1;
                            if (index < currentDirs.size()) {
                                selectedIndices.emplace(index);
                            } else {
                                errorMessages.emplace("Invalid index: '" + token + "'.");
                            }
                        }
                    } catch (const std::invalid_argument&) {
                        errorMessages.emplace("Invalid input: '" + token + "'.");
                    }
                }

                for (size_t index : selectedIndices) {
                    selectedIsoDirs.push_back(currentDirs[index]);
                }
            }

            if (!selectedIsoDirs.empty()) {
                unsigned int numThreads = std::min(static_cast<int>(selectedIsoDirs.size()), static_cast<int>(maxThreads));
                ThreadPool pool(numThreads);
                std::vector<std::future<void>> futuresUmount;
                futuresUmount.reserve(numThreads);

                size_t batchSize = (selectedIsoDirs.size() + maxThreads - 1) / maxThreads;
                std::vector<std::vector<std::string>> batches;
                for (size_t i = 0; i < selectedIsoDirs.size(); i += batchSize) {
                    batches.emplace_back(selectedIsoDirs.begin() + i, std::min(selectedIsoDirs.begin() + i + batchSize, selectedIsoDirs.end()));
                }

                std::atomic<int> completedIsos(0);
                int totalIsos = static_cast<int>(selectedIsoDirs.size());
                std::atomic<bool> isComplete(false);

                std::thread progressThread(displayProgressBar, std::ref(completedIsos), std::cref(totalIsos), std::ref(isComplete));

                for (auto& batch : batches) {
                    std::lock_guard<std::mutex> highLock(Mutex4High);
                    futuresUmount.emplace_back(pool.enqueue([batch = std::move(batch), &unmountedFiles, &unmountedErrors, &completedIsos]() {
                        for (const auto& iso : batch) {
                            unmountISO({iso}, unmountedFiles, unmountedErrors);
                            completedIsos.fetch_add(1, std::memory_order_relaxed);
                        }
                    }));
                }

                for (auto& future : futuresUmount) {
                    future.wait();
                }

                isComplete.store(true);
                progressThread.join();

                if (verbose) {
                    printUnmountedAndErrors(unmountedFiles, unmountedErrors, errorMessages);
                    std::cout << "\n\n\033[1;32m↵ to continue...\033[0;1m";
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                } else {
                    unmountedFiles.clear();
                    unmountedErrors.clear();
                    errorMessages.clear();
                }

                clearScrollBuffer();
                isFiltered = false;
            } else {
                clearScrollBuffer();
                std::cerr << "\n\033[1;91mNo valid input provided for umount.\n";
                std::cout << "\n\033[1;32m↵ to continue...";
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            }
        }
    }
}
