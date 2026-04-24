// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../themes.h"

/**
 * @brief Primary entry point for handling mount/umount CLI commands.
 * * This function parses command-line arguments to facilitate batch mounting 
 * or unmounting of ISO files. It supports recursion depth limits, 
 * silent mode operation, and handles root privilege verification.
 * * @param argc Argument count from main.
 * @param argv Argument vector from main.
 * @return int Status code (0 for success, 1 for error).
 */
int handleMountUmountCommands(int argc, char* argv[]) {
    setupSignalHandlerCancellations();
    g_operationCancelled.store(false);

    if (argc < 2) {
        std::cerr << UI::Palette::Red << "Error: No arguments provided." << UI::Palette::Reset << "\n\n";
        return 1;
    }

    bool silentMode = false;
    int maxDepth = -1; 
    std::vector<std::string> args;
    args.reserve(argc);

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--silent") {
            silentMode = true;
        } else if (arg.substr(0, 2) == "-d") {
            try {
                maxDepth = std::stoi(arg.substr(2));
                if (maxDepth < 0) {
                    std::cerr << UI::Palette::Yellow << "Warning: Negative depth (" << maxDepth
                              << ") means a full recursive scan." << UI::Palette::Reset << "\n";
                    maxDepth = -1;
                }
            } catch (...) {
                std::cerr << UI::Palette::Yellow << "Warning: Invalid depth '" << arg.substr(2)
                          << "'. Using 0 (surface scan)." << UI::Palette::Reset << "\n";
                maxDepth = 0;
            }
        } else {
            args.push_back(arg);
        }
    }

    if (args.empty()) {
        std::cerr << UI::Palette::Red << "Error: No action provided." << UI::Palette::Reset << "\n\n";
        return 1;
    }

    std::string action = args.back();

    if (action == "mount") {
        if (geteuid() != 0) {
            std::cerr << UI::Palette::Red << "Error: Root privileges required for mounting ISOs." << UI::Palette::Reset << "\n\n";
            return 1;
        }

        std::unordered_set<std::string> isoFiles;
        bool hasErrors = false;

        for (size_t i = 0; i < args.size() - 1; ++i) {
            if (g_operationCancelled.load()) {
                if (!silentMode) std::cout << UI::Palette::Yellow << "\nOperation cancelled by user." << UI::Palette::Reset << "\n\n";
                return 1;
            }

            fs::path path(args[i]);
            std::string originalPath = path.string();

            try {
                if (!fs::exists(path)) {
                    if (!silentMode)
                        std::cerr << UI::Palette::Yellow << "Warning: '" << UI::Palette::Red << originalPath
                                  << UI::Palette::Yellow << "' does not exist, skipping." << UI::Palette::Reset << "\n";
                    hasErrors = true;
                    continue;
                }

                if (fs::is_regular_file(path)) {
                    std::string ext = path.extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext != ".iso") {
                        if (!silentMode)
                            std::cerr << UI::Palette::Yellow << "Warning: '" << UI::Palette::Red << originalPath
                                      << UI::Palette::Yellow << "' is not an ISO file, skipping." << UI::Palette::Reset << "\n";
                        hasErrors = true;
                        continue;
                    }
                    isoFiles.insert(fs::canonical(path).string());
                } else if (fs::is_directory(path)) {
                    disableInput();
                    if (!silentMode) {
                        std::cout << UI::Palette::BoldReset << "Scanning directory " << path << " ("<< (maxDepth == 0 ? "surface scan" : "max depth: " + (maxDepth < 0 ? "unlimited" : std::to_string(maxDepth))) << ")...\n";
                    }

                    std::function<void(const fs::path&, int)> scanDir;
                    scanDir = [&](const fs::path& dir, int currentDepth) {
                        try {
                            for (const auto& entry : fs::directory_iterator(dir)) {
                                if (g_operationCancelled.load()) return;
                                if (entry.is_symlink()) continue; 

                                if (entry.is_regular_file()) {
                                    std::string ext = entry.path().extension().string();
                                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                                    if (ext == ".iso") {
                                        isoFiles.insert(fs::canonical(entry.path()).string());
                                    }
                                }
                                else if (entry.is_directory() && (maxDepth == -1 || currentDepth < maxDepth)) {
                                    scanDir(entry.path(), currentDepth + 1);
                                }
                            }
                        } catch (const fs::filesystem_error& e) {
                            if (!silentMode)
                                std::cerr << UI::Palette::Yellow << "Warning: Error scanning directory '" << UI::Palette::Red << dir.string()
                                          << UI::Palette::Yellow << "': " << e.what() << UI::Palette::Reset << "\n";
                            hasErrors = true;
                        }
                    };
                    
                    scanDir(path, 0);
                } else {
                    if (!silentMode)
                        std::cerr << UI::Palette::Yellow << "Warning: '" << UI::Palette::Red << originalPath
                                  << UI::Palette::Yellow << "' is not a valid file or directory, skipping." << UI::Palette::Reset << "\n";
                    hasErrors = true;
                }
            } catch (const fs::filesystem_error& e) {
                if (!silentMode)
                    std::cerr << UI::Palette::Yellow << "Warning: Error processing '" << UI::Palette::Red << originalPath
                              << UI::Palette::Yellow << "': " << e.what() << UI::Palette::Reset << "\n";
                hasErrors = true;
            }
        }
        
        if (!silentMode && g_operationCancelled.load()) std::cout << UI::Palette::Yellow << "Mount Operation cancelled by user." << UI::Palette::Reset << "\n";
        
        if (isoFiles.empty()) {
            if (!silentMode && !g_operationCancelled.load()) std::cout << "\n" << UI::Palette::Yellow << "No ISO files found to mount." << UI::Palette::Reset << "\n";
            return hasErrors ? 1 : 0;
        }

        if (!silentMode) {
            std::cout << "\nLocated " << isoFiles.size() << " ISO file" << (isoFiles.size() == 1 ? "" : "s") << "; Attempting to mount...\n";
        }

        std::unordered_set<std::string> mountedFiles;
        std::unordered_set<std::string> skippedMessages;
        std::unordered_set<std::string> mountedFails;
        std::atomic<size_t> completedTasks{0};
        std::atomic<size_t> failedTasks{0};

        mountIsoFiles(std::vector<std::string>(isoFiles.begin(), isoFiles.end()),
                      mountedFiles, skippedMessages, mountedFails,
                      &completedTasks, &failedTasks, silentMode);

        if (!silentMode) {
            for (const auto& msg : mountedFiles) std::cout << msg << "\n";
            for (const auto& msg : skippedMessages) std::cout << msg << "\n";
            for (const auto& msg : mountedFails) std::cout << msg << "\n";
            std::cout << "\nMount Summary:\n";
            std::cout << "Successful: " << completedTasks.load() << "\n";
            std::cout << "Failed: " << failedTasks.load() << UI::Palette::Reset << "\n\n" ;
        }

        return (completedTasks.load() > 0 || (!hasErrors && isoFiles.empty())) ? 0 : 1;
    }

    else if (action == "umount" || action == "unmount") {
        std::unordered_set<std::string> mountPointsToUnmount; 
        bool hasErrors = false;
        
        if (geteuid() != 0) {
            std::cerr << UI::Palette::Red << "Error: Root privileges required for unmounting ISOs." << UI::Palette::Reset << "\n\n";
            return 1;
        }

        if (args.size() <= 1 || (args.size() == 2 && (args[0] == "all"))) {
            disableInput();
            if (!silentMode) std::cout << UI::Palette::BoldReset << "Scanning /mnt for ISO mount points (surface scan)...\n";
            try {
                for (const auto& entry : fs::directory_iterator("/mnt")) {
                    if (g_operationCancelled.load()) {
                        if (!silentMode) std::cout << UI::Palette::Yellow << "\nOperation cancelled by user." << UI::Palette::Reset << "\n\n";
                        return 1;
                    }
                    if (entry.is_directory()) {
                        std::string dirName = entry.path().filename().string();
                        if (dirName.substr(0, 4) == "iso_") {
                            mountPointsToUnmount.insert(fs::canonical(entry.path()).string());
                        }
                    }
                }
            } catch (const fs::filesystem_error& e) {
                std::cerr << UI::Palette::Red << "Error scanning /mnt: " << e.what() << UI::Palette::Reset << "\n\n";
                return 1;
            }
        } else {
            for (size_t i = 0; i < args.size() - 1; ++i) {
                if (g_operationCancelled.load()) {
                    if (!silentMode) std::cout << UI::Palette::Yellow << "\nOperation cancelled by user." << UI::Palette::Reset << "\n\n";
                    return 1;
                }

                fs::path path(args[i]);
                std::string originalPath = path.string();

                try {
                    if (fs::exists(path) && fs::is_directory(path)) {
                        auto canonicalPath = fs::canonical(path);
                        std::string canonicalStr = canonicalPath.string();

                        if (canonicalStr == "/mnt") {
                            disableInput();
                            if (!silentMode) std::cout << "Scanning /mnt for ISO mount points (surface scan)...\n";
                            for (const auto& entry : fs::directory_iterator(canonicalPath)) {
                                if (entry.is_directory()) {
                                    std::string entryDirName = entry.path().filename().string();
                                    if (entryDirName.rfind("iso_", 0) == 0) {
                                        mountPointsToUnmount.insert(fs::canonical(entry.path()).string());
                                    }
                                }
                            }
                        } else if (canonicalStr.rfind("/mnt/iso_", 0) == 0) {
                            mountPointsToUnmount.insert(canonicalStr);
                        } else {
                            if (!silentMode)
                                std::cerr << UI::Palette::Yellow << "Warning: Directory parameter '" << UI::Palette::Red << originalPath
                                          << UI::Palette::Yellow << "' is not allowed. Only " << UI::Palette::Blue << "/mnt" << UI::Palette::Yellow 
                                          << " or " << UI::Palette::Blue << "/mnt/iso_*" << UI::Palette::Yellow << " allowed." << UI::Palette::Reset << "\n";
                            hasErrors = true;
                        }
                    } else {
                        fs::path candidatePath = path;
                        if (path.is_relative()) {
                            candidatePath = fs::path("/mnt") / ("iso_" + path.filename().string());
                        }

                        if (fs::exists(candidatePath) && fs::is_directory(candidatePath)) {
                            mountPointsToUnmount.insert(fs::canonical(candidatePath).string());
                        } else {
                            if (!silentMode)
                                std::cerr << UI::Palette::Yellow << "Warning: Mount point '" << UI::Palette::Red << originalPath
                                          << UI::Palette::Yellow << "' does not exist or is invalid, skipping." << UI::Palette::Reset << "\n";
                            hasErrors = true;
                        }
                    }
                } catch (const fs::filesystem_error& e) {
                    if (!silentMode)
                        std::cerr << UI::Palette::Yellow << "Warning: Error processing '" << UI::Palette::Red << originalPath
                                  << UI::Palette::Yellow << "': " << e.what() << UI::Palette::Reset << "\n";
                    hasErrors = true;
                }
            }
        }
        
        if (!silentMode && g_operationCancelled.load()) std::cout << UI::Palette::Yellow << "Umount Operation cancelled by user." << UI::Palette::Reset << "\n\n";
        
        if (mountPointsToUnmount.empty()) {
            if (!silentMode && !g_operationCancelled.load()) std::cout << "\n" << UI::Palette::Yellow << "No ISO mount points found to unmount." << UI::Palette::Reset << "\n\n";
            return hasErrors ? 1 : 0;
        }

        if (!silentMode) std::cout << UI::Palette::BoldReset << "\nLocated " << mountPointsToUnmount.size() << " mount point" << (mountPointsToUnmount.size() == 1 ? "" : "s") << "; Attempting to unmount...\n";
        std::unordered_set<std::string> unmountedFiles;
        std::unordered_set<std::string> unmountedErrors;
        std::atomic<size_t> completedTasks{0};
        std::atomic<size_t> failedTasks{0};

        unmountISO(std::vector<std::string>(mountPointsToUnmount.begin(), mountPointsToUnmount.end()),
                   unmountedFiles, unmountedErrors,
                   &completedTasks, &failedTasks, silentMode);

        if (!silentMode) {
            for (const auto& msg : unmountedFiles) std::cout << msg << "\n";
            for (const auto& msg : unmountedErrors) std::cout << msg << "\n";
            std::cout << "\nUnmount Summary:\n";
            std::cout << "Successful: " << completedTasks.load() << "\n";
            std::cout << "Failed: " << failedTasks.load() << UI::Palette::Reset << "\n\n";
        }
		
        return (failedTasks.load() == 0 && !hasErrors) || completedTasks.load() > 0 ? 0 : 1;
    }

    std::cerr << UI::Palette::Red << "Error: Unknown action '" << action << "'" << UI::Palette::Reset << "\n\n";
    return 1;
}
