#include "../headers.h"


int handleMountUmountCommands(int argc, char* argv[]) {
    // Setup signal handler
    setupSignalHandlerCancellations();
    g_operationCancelled.store(false);

    if (argc < 2) {
        std::cerr << "\033[1;91mError: No arguments provided.\n\033[0m";
        return 1;
    }

    // Parse flags
    bool silentMode = false;
    int maxDepth = -1; // Default: max depth
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
                    std::cerr << "\033[1;93mWarning: Negative depth (" << maxDepth
                              << ") means a full recursive scan.\n\033[0m"; // Fixed typo: recursive
                    maxDepth = -1;
                }
            } catch (...) {
                std::cerr << "\033[1;93mWarning: Invalid depth '" << arg.substr(2)
                          << "'. Using 0 (surface scan).\n\033[0m";
                maxDepth = 0;
            }
        } else {
            args.push_back(arg);
        }
    }

    if (args.empty()) {
        std::cerr << "\033[1;91mError: No action provided.\n\033[0m";
        return 1;
    }

    std::string action = args.back();

    // ---------- MOUNT MULTIPLE ----------
    if (action == "mount") {
        if (geteuid() != 0) {
            std::cerr << "\033[1;91mError: Root privileges required for mounting ISOs.\n\033[0m";
            return 1;
        }

        std::unordered_set<std::string> isoFiles;
        bool hasErrors = false;

        // Collect all args except the last one (which is "mount")
        for (size_t i = 0; i < args.size() - 1; ++i) {
            if (g_operationCancelled.load()) {
                if (!silentMode) std::cout << "\033[1;33m\nOperation cancelled by user.\n\033[0m";
                return 1;
            }

            fs::path path(args[i]);
            std::string originalPath = path.string();

            try {
                if (!fs::exists(path)) {
                    if (!silentMode)
                        std::cerr << "\033[1;93mWarning: '\033[1;91m" << originalPath
                                  << "\033[1;93m' does not exist, skipping.\n\033[0m";
                    hasErrors = true;
                    continue;
                }

                if (fs::is_regular_file(path)) {
                    std::string ext = path.extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext != ".iso") {
                        if (!silentMode)
                            std::cerr << "\033[1;93mWarning: '\033[1;91m" << originalPath
                                      << "\033[1;93m' is not an ISO file, skipping.\n\033[0m";
                        hasErrors = true;
                        continue;
                    }
                    isoFiles.insert(fs::canonical(path).string());
                } else if (fs::is_directory(path)) {
					disableInput();
                    if (!silentMode) {
						std::cout << "Scanning directory " << path << " ("<< (maxDepth == 0 ? "surface scan" : "max depth: " + std::string(maxDepth < 0 ? "unlimited" : std::to_string(maxDepth))) << ")...\n";
					}

                    std::function<void(const fs::path&, int)> scanDir;
                    scanDir = [&](const fs::path& dir, int currentDepth) {
                        try {
                            for (const auto& entry : fs::directory_iterator(dir)) {
                                if (g_operationCancelled.load()) return;
                                if (entry.is_symlink()) continue;  // <-- skip symlinks

                                if (entry.is_regular_file()) {
                                    std::string ext = entry.path().extension().string();
                                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                                    if (ext == ".iso") {
                                        isoFiles.insert(fs::canonical(entry.path()).string());
                                    }
                                }
                                // Allow recursion when maxDepth is -1 (unlimited) or within depth limit
                                else if (entry.is_directory() && (maxDepth == -1 || currentDepth < maxDepth)) {
                                    scanDir(entry.path(), currentDepth + 1);
                                }
                            }
                        } catch (const fs::filesystem_error& e) {
                            if (!silentMode)
                                std::cerr << "\033[1;93mWarning: Error scanning directory '\033[1;91m" << dir
                                          << "\033[1;93m': " << e.what() << "\n\033[0m";
                            hasErrors = true;
                        }
                    };
					
                    scanDir(path, 0);
                } else {
                    if (!silentMode)
                        std::cerr << "\033[1;93mWarning: '\033[1;91m" << originalPath
                                  << "\033[1;93m' is not a valid file or directory, skipping.\n\033[0m";
                    hasErrors = true;
                }
            } catch (const fs::filesystem_error& e) {
                if (!silentMode)
                    std::cerr << "\033[1;93mWarning: Error processing '\033[1;91m" << originalPath
                              << "\033[1;93m': " << e.what() << "\n\033[0m";
                hasErrors = true;
            }
        }
		
		if (!silentMode && g_operationCancelled.load()) std::cout << "\033[1;33mMount Operation cancelled by user.\n\033[0m";
		
        if (isoFiles.empty()) {
            if (!silentMode && !g_operationCancelled.load()) std::cout << "No ISO files found to mount.\n";
            return hasErrors ? 1 : 0;
        }

        if (!silentMode) std::cout << "\nLocated " << isoFiles.size() << " ISO files; Attempting to mount...\n";
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
        }

        if (!silentMode) {
            std::cout << "\nMount Summary:\n";
            std::cout << "Successful: " << completedTasks.load() << "\n";
            std::cout << "Failed: " << failedTasks.load() << "\n";
        }

        return (completedTasks.load() > 0 || (!hasErrors && isoFiles.empty())) ? 0 : 1;
    }

    // ---------- UMOUNT MULTIPLE ----------
    else if (action == "umount" || action == "unmount") {
        std::unordered_set<std::string> mountPointsToUnmount; // Automatic deduplication
        bool hasErrors = false;

        // If no arguments before "umount" or if explicitly "all"
        if (args.size() <= 1 || (args.size() == 2 && (args[0] == "all"))) {
			disableInput();
            if (!silentMode) std::cout << "Scanning /mnt for ISO mount points (surface scan)...\n";
            try {
                for (const auto& entry : fs::directory_iterator("/mnt")) {
                    if (g_operationCancelled.load()) {
                        if (!silentMode) std::cout << "\033[1;33m\nOperation cancelled by user.\n\033[0m";
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
                std::cerr << "\033[1;91mError scanning /mnt: " << e.what() << "\n\033[0m";
                return 1;
            }
        } else {
            // Reject directory parameters other than /mnt
            for (size_t i = 0; i < args.size() - 1; ++i) {
                if (g_operationCancelled.load()) {
                    if (!silentMode) std::cout << "\033[1;93m\nOperation cancelled by user.\n\033[0m";
                    return 1;
                }

                fs::path path(args[i]);
                std::string originalPath = path.string();

                try {
                    // Reject if directory parameter and not /mnt or /mnt/iso_*
                    if (fs::exists(path) && fs::is_directory(path)) {
                        auto canonicalPath = fs::canonical(path);
                        std::string canonicalStr = canonicalPath.string();

                        // Allowed directories: /mnt or /mnt/iso_*
                        if (canonicalStr == "/mnt") {
							disableInput();
                            // Surface scan for iso_ dirs in /mnt
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
                            // Accept specific /mnt/iso_* dir
                            mountPointsToUnmount.insert(canonicalStr);
                        } else {
                            if (!silentMode)
                                std::cerr << "\033[1;93mWarning: Directory parameter '\033[1;91m" << originalPath
                                          << "\033[1;93m' is not allowed. Only \033[1;94m/mnt\033[1;93m or \033[1;94m/mnt/iso_*\033[1;93m allowed.\n\033[0m";
                            hasErrors = true;
                        }
                    } else {
                        // For relative paths or non-directory paths, treat as mount point name under /mnt/iso_*
                        // Compose full path:
                        fs::path candidatePath = path;
                        if (path.is_relative()) {
                            candidatePath = fs::path("/mnt") / ("iso_" + path.filename().string());
                        }

                        if (fs::exists(candidatePath) && fs::is_directory(candidatePath)) {
                            mountPointsToUnmount.insert(fs::canonical(candidatePath).string());
                        } else {
                            if (!silentMode)
                                std::cerr << "\033[1;93mWarning: Mount point '\033[1;91m" << originalPath
                                          << "\033[1;93m' does not exist or is invalid, skipping.\n\033[0m";
                            hasErrors = true;
                        }
                    }
                } catch (const fs::filesystem_error& e) {
                    if (!silentMode)
                        std::cerr << "\033[1;93mWarning: Error processing '\033[1;91m" << originalPath
                                  << "\033[1;93m': " << e.what() << "\n\033[0m";
                    hasErrors = true;
                }
            }
        }
		if (!silentMode && g_operationCancelled.load()) std::cout << "\033[1;33mUmount Operation cancelled by user.\n\033[0m";
		
        if (mountPointsToUnmount.empty()) {
            if (!silentMode && !g_operationCancelled.load()) std::cout << "No ISO mount points found to unmount.\n";
            return hasErrors ? 1 : 0;
        }

        if (!silentMode) std::cout << "Unmounting " << mountPointsToUnmount.size() << " mount point(s)...\n";
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
        }

        if (!silentMode) {
            std::cout << "\nUnmount Summary:\n";
            std::cout << "Successful: " << completedTasks.load() << "\n";
            std::cout << "Failed: " << failedTasks.load() << "\n";
        }

        return (failedTasks.load() == 0 && !hasErrors) || completedTasks.load() > 0 ? 0 : 1;
    }

    std::cerr << "\033[1;91mError: Unknown action '" << action << "'\n\033[0m";
    return 1;
}
