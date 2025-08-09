#include "../headers.h"

int handleMountUmountCommands(int argc, char* argv[]) {
    // Setup signal handler
    setupSignalHandlerCancellations();
    g_operationCancelled.store(false);

    if (argc < 2) {
        std::cerr << "\033[1;91mError: No arguments provided.\n\033[0m";
        return 1;
    }

    // Check for --quiet flag
    bool quietMode = false;
    std::vector<std::string> args;
    args.reserve(argc);
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--quiet") {
            quietMode = true;
        } else {
            args.push_back(arg);
        }
    }

    if (args.empty()) {
        std::cerr << "\033[1;91mError: No action provided.\n\033[0m";
        return 1;
    }

    std::string action = args.back(); // Last arg is always the action

    // ---------- MOUNT MULTIPLE ----------
    if (action == "mount") {
        if (geteuid() != 0) {
            std::cerr << "\033[1;91mError: Root privileges required for mounting ISOs.\n\033[0m";
            return 1;
        }

        std::vector<std::string> isoFiles;
        bool hasErrors = false;

        // Collect all args except the last one (which is "mount")
        for (size_t i = 0; i < args.size() - 1; ++i) {
            std::string path = args[i];
            if (!fs::exists(path)) {
                if (!quietMode) std::cerr << "\033[1;93mWarning: '\033[1;91m" << path << "\033[1;93m' does not exist, skipping.\n\033[0m";
                hasErrors = true;
                continue; // Continue processing other paths
            }

            if (fs::is_regular_file(path)) {
                std::string ext = fs::path(path).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext != ".iso") {
                    if (!quietMode) std::cerr << "\033[1;93m '\033[1;91m" << path << "\033[1;93m' is not an ISO file, skipping.\n";
                    hasErrors = true;
                    continue; // Continue processing other paths
                }
                isoFiles.push_back(path);
            }
            else if (fs::is_directory(path)) {
                if (!quietMode) std::cout << "Scanning directory " << path << " for ISO files...\n";
                try {
                    for (const auto& entry : fs::recursive_directory_iterator(path)) {
                        if (g_operationCancelled.load()) {
                            if (!quietMode) std::cout << "\033[1;93m\nScan cancelled by user.\n\033[0m";
                            return 1;
                        }
                        if (entry.is_regular_file()) {
                            std::string ext = entry.path().extension().string();
                            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                            if (ext == ".iso") {
                                isoFiles.push_back(entry.path().string());
                                if (!quietMode) std::cout << "Found: " << entry.path().string() << "\n";
                            }
                        }
                    }
                }
                catch (const fs::filesystem_error& e) {
                    if (!quietMode) std::cerr << "\033[1;93mWarning: Error scanning directory '\033[1;91m" << path << "\033[1;93m': " << e.what() << "\n\033[0m";
                    hasErrors = true;
                    continue; // Continue processing other paths
                }
            }
            else {
                if (!quietMode) std::cerr << "\033[1;93mWarning: '\033[1;91m" << path << "\033[1;93m' is not a valid file or directory, skipping.\n\033[0m";
                hasErrors = true;
                continue; // Continue processing other paths
            }
        }

        if (isoFiles.empty()) {
            if (!quietMode) std::cout << "No ISO files found to mount.\n";
            return hasErrors ? 1 : 0; // Return error if there were invalid paths but no valid files
        }

        if (!quietMode) std::cout << "\nMounting ISO files...\n";
        std::unordered_set<std::string> mountedFiles;
        std::unordered_set<std::string> skippedMessages;
        std::unordered_set<std::string> mountedFails;
        std::atomic<size_t> completedTasks{0};
        std::atomic<size_t> failedTasks{0};

        mountIsoFiles(isoFiles, mountedFiles, skippedMessages, mountedFails,
                      &completedTasks, &failedTasks);

        if (!quietMode) {
			for (const auto& msg : mountedFiles) std::cout << msg << "\n";
			for (const auto& msg : skippedMessages) std::cout << msg << "\n";
			for (const auto& msg : mountedFails) std::cout << msg << "\n";
		}

        if (!quietMode) {
            std::cout << "\nMount Summary:\n";
            std::cout << "Successful: " << completedTasks.load() << "\n";
            std::cout << "Failed: " << failedTasks.load() << "\n";
        }

        // Return success if at least one mount succeeded, or if there were no errors and no files to process
        return (completedTasks.load() > 0 || (!hasErrors && isoFiles.empty())) ? 0 : 1;
    }

    // ---------- UMOUNT MULTIPLE ----------
    else if (action == "umount" || action == "unmount") {
        std::vector<std::string> mountPointsToUnmount;
        bool hasErrors = false;

        // If only one argument before "umount" and it is "all" or "*"
        if (args.size() == 2 && (args[0] == "all" || args[0] == "*")) {
            if (!quietMode) std::cout << "Scanning /mnt for ISO mount points...\n";
            try {
                for (const auto& entry : fs::directory_iterator("/mnt")) {
                    if (g_operationCancelled.load()) {
                        if (!quietMode) std::cout << "\033[1;93m\nScan cancelled by user.\n\033[0m";
                        return 1;
                    }
                    if (entry.is_directory()) {
                        std::string dirName = entry.path().filename().string();
                        if (dirName.substr(0, 4) == "iso_") {
                            mountPointsToUnmount.push_back(entry.path().string());
                        }
                    }
                }
            }
            catch (const fs::filesystem_error& e) {
                std::cerr << "\033[1;91mError scanning /mnt: " << e.what() << "\n\033[0m";
                return 1;
            }
        }
        else {
            // Loop through all paths before "umount"
            for (size_t i = 0; i < args.size() - 1; ++i) {
                std::string path = args[i];
                std::string originalPath = path;

                if (!fs::exists(path)) {
                    // If just given name, assume it's in /mnt
                    if (path.find('/') == std::string::npos) {
                        if (path.substr(0, 4) != "iso_") path = "iso_" + path;
                        path = "/mnt/" + path;
                    }
                }

                if (!fs::exists(path)) {
                    if (!quietMode) std::cerr << "\033[1;93mWarning: Mount point '\033[1;91m" << originalPath << "\033[1;93m' does not exist, skipping.\n\033[0m";
                    hasErrors = true;
                    continue; // Continue processing other paths
                }

                while (!path.empty() && path.back() == '/') {
                    path.pop_back();
                }

                if (fs::is_directory(path)) {
                    std::string dirName = fs::path(path).filename().string();
                    if (dirName.rfind("iso_", 0) == 0) {
                        mountPointsToUnmount.push_back(path);
                    }
                    else {
                        try {
                            for (const auto& entry : fs::directory_iterator(path)) {
                                if (entry.is_directory()) {
                                    std::string entryDirName = entry.path().filename().string();
                                    if (entryDirName.rfind("iso_", 0) == 0) {
                                        mountPointsToUnmount.push_back(entry.path().string());
                                    }
                                }
                            }
                        }
                        catch (const fs::filesystem_error& e) {
                            if (!quietMode) std::cerr << "\033[1;93mWarning: Error scanning directory '\033[1;91m" << path << "\033[1;93m': " << e.what() << "\n\033[0m";
                            hasErrors = true;
                            continue;
                        }
                    }
                }
                else {
                    if (!quietMode) std::cerr << "\033[1;93mWarning: '\033[1;91m" << originalPath << "\033[1;93m' is not a directory, skipping.\n\033[0m";
                    hasErrors = true;
                    continue; // Continue processing other paths
                }
            }
        }

        if (mountPointsToUnmount.empty()) {
            if (!quietMode) std::cout << "No ISO mount points found to unmount.\n";
            return hasErrors ? 1 : 0; // Return error if there were invalid paths but no valid mount points
        }

        if (!quietMode) std::cout << "Unmounting " << mountPointsToUnmount.size() << " mount point(s)...\n";
        std::unordered_set<std::string> unmountedFiles;
        std::unordered_set<std::string> unmountedErrors;
        std::atomic<size_t> completedTasks{0};
        std::atomic<size_t> failedTasks{0};

        unmountISO(mountPointsToUnmount, unmountedFiles, unmountedErrors,
                   &completedTasks, &failedTasks);
		if (!quietMode) {
			for (const auto& msg : unmountedFiles) std::cout << msg << "\n";
			for (const auto& msg : unmountedErrors) std::cout << msg << "\n";
		}
        if (!quietMode) {
            std::cout << "\nUnmount Summary:\n";
            std::cout << "Successful: " << completedTasks.load() << "\n";
            std::cout << "Failed: " << failedTasks.load() << "\n";
        }

        // Return success if no tasks failed and there were no input errors, or if at least one unmount succeeded
        return (failedTasks.load() == 0 && !hasErrors) || completedTasks.load() > 0 ? 0 : 1;
    }

    // ---------- UNKNOWN ----------
    else {
        std::cerr << "Unknown action: " << action << "\n";
        std::cerr << "Usage:\n";
        std::cerr << "  " << argv[0] << " [--quiet] <file|dir> [...] mount\n";
        std::cerr << "  " << argv[0] << " [--quiet] <mount_point|dir|all> [...] umount\n";
        return 1;
    }
}

