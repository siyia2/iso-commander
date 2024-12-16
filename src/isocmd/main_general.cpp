// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"


// Get max available CPU cores for global use, fallback is 2 cores
unsigned int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;

// Default readline history save path
const std::string historyFilePath = std::string(getenv("HOME")) + "/.cache/iso_commander_history_cache.txt";
const std::string historyPatternFilePath = std::string(getenv("HOME")) + "/.cache/iso_commander_pattern_cache.txt";

//Maximum number of history entries at a time
const int MAX_HISTORY_LINES = 25;

const int MAX_HISTORY_PATTERN_LINES = 25;

//For toggling long/short paths in lists and verbose
bool toggleFullList = false;

// For memory mapping string transformations
std::unordered_map<std::string, std::string> transformationCache;

// Global variables for cleanup
int lockFileDescriptor = -1;


// Main function
int main(int argc, char *argv[]) {
	// For enabling/disabling cache refresh prompt
	bool promptFlag = true;
	// For saving history to a differrent cache for FilterPatterns
	bool historyPattern = false;
	//Flag to enable/disable verboseoutput
	bool verbose = false;
	// Traverse depth for cache refresh
	int maxDepth = -1;
	
	if (argc == 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-v")) {
        printVersionNumber("5.4.5");
        return 0;
    }

    const char* lockFile = "/tmp/isocmd.lock";

    lockFileDescriptor = open(lockFile, O_CREAT | O_RDWR, 0666);

    if (lockFileDescriptor == -1) {
        std::cerr << "\033[93mAnother instance of isocmd is already running. If not run \"rm /tmp/isocmd.lock\".\n\033[0m";
        return 1;
    }

    struct flock fl;
    fl.l_type = F_WRLCK;  // Write lock
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;  // Lock the whole file

    if (fcntl(lockFileDescriptor, F_SETLK, &fl) == -1) {
        std::cerr << "\033[93mAnother instance of isocmd is already running.\n\033[0m";
        close(lockFileDescriptor);
        return 1;
    }

    // Register signal handlers
    signal(SIGINT, signalHandler);  // Handle Ctrl+C
    signal(SIGTERM, signalHandler); // Handle termination signals

    bool exitProgram = false;
    std::string choice;

    while (!exitProgram) {
		// Calls prevent_clear_screen and tab completion
		rl_bind_key('\f', prevent_clear_screen_and_tab_completion);
		rl_bind_key('\t', prevent_clear_screen_and_tab_completion);

		globalIsoFileList.reserve(100);
        clearScrollBuffer();
        print_ascii();
        // Display the main menu options
        printMenu();

        // Clear history
        clear_history();

        // Prompt for the main menu choice
        char* rawInput = readline("\n\001\033[1;94m\002Choose an option:\001\033[0;1m\002 ");

        std::unique_ptr<char[], decltype(&std::free)> input(rawInput, &std::free);

        std::string mainInputString(input.get());

        if (!input.get()) {
            break; // Exit the program if readline returns NULL (e.g., on EOF or Ctrl+D)
        }


        std::string choice(mainInputString);

        if (choice == "1") {
            submenu1(promptFlag, maxDepth, historyPattern, verbose);
        } else {
            // Check if the input length is exactly 1
            if (choice.length() == 1) {
                switch (choice[0]) {
                    case '2':
                        submenu2(promptFlag, maxDepth, historyPattern, verbose);
                        break;
                    case '3':
                        manualRefreshCache("", promptFlag, maxDepth, historyPattern);
                        clearScrollBuffer();
                        break;
                    case '4':
                        exitProgram = true; // Exit the program
                        clearScrollBuffer();
                        break;
                    default:
                        break;
                }
            }
        }
    }

    close(lockFileDescriptor); // Close the file descriptor, releasing the lock
    unlink(lockFile); // Remove the lock file
    return 0;
}

// ART&MENUS

// ... Function definitions ...

// Print the version number of the program
void printVersionNumber(const std::string& version) {

    std::cout << "\x1B[32mIso Commander v" << version << "\x1B[0m\n"; // Output the version number in green color
}


// Function to print ascii
void print_ascii() {
    // Display ASCII art

    const char* Color = "\x1B[1;38;5;214m";
    const char* resetColor = "\x1B[0m"; // Reset color to default

std::cout << Color << R"((   (       )            )    *      *              ) (         (
 )\ ))\ ) ( /(     (  ( /(  (  `   (  `    (     ( /( )\ )      )\ )
(()/(()/( )\())    )\ )\()) )\))(  )\))(   )\    )\()(()/(  (  (()/(
 /(_)/(_)((_)\   (((_((_)\ ((_)()\((_)()((((_)( ((_)\ /(_)) )\  /(_))
(_))(_))   ((_)  )\___ ((_)(_()((_(_()((_)\ _ )\ _((_(_))_ ((_)(_))
|_ _/ __| / _ \ ((/ __/ _ \|  \/  |  \/  (_)_\(_| \| ||   \| __| _ \
 | |\__ \| (_) | | (_| (_) | |\/| | |\/| |/ _ \ | .` || |) | _||   /
|___|___/ \___/   \___\___/|_|  |_|_|  |_/_/ \_\|_|\_||___/|___|_|_\

)" << resetColor;

}


// Function to print submenu1
void submenu1(bool& promptFlag, int& maxDepth, bool& historyPattern, bool& verbose) {
	
    while (true) {
		// Calls prevent_clear_screen and tab completion
		rl_bind_key('\f', prevent_clear_screen_and_tab_completion);
		rl_bind_key('\t', prevent_clear_screen_and_tab_completion);
		
        clearScrollBuffer();
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|↵ Manage ISO              |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|1. Mount                 |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|2. Unmount               |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|3. Delete                |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|4. Move                  |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|5. Copy                  |\n";
        std::cout << "\033[1;32m+-------------------------+";
        std::cout << "\n";
        char* rawInput = readline("\n\001\033[1;94m\002Choose an option:\001\033[0;1m\002 ");

        // Use std::unique_ptr to manage memory for input
		std::unique_ptr<char[], decltype(&std::free)> input(rawInput, &std::free);

        std::string mainInputString(input.get());

        if (!input.get() || std::strlen(input.get()) == 0) {
			break; // Exit the submenu if input is empty or NULL
		}

          std::string submenu_choice(mainInputString);
         // Check if the input length is exactly 1
        if (submenu_choice.empty() || submenu_choice.length() == 1) {
		std::string operation;
		switch (submenu_choice[0]) {
        case '1':
			clearScrollBuffer();
            select_and_mount_files_by_number(historyPattern, verbose);
            clearScrollBuffer();
            break;
        case '2':
			clearScrollBuffer();
            unmountISOs(historyPattern, verbose);
            clearScrollBuffer();
            break;
        case '3':
			clearScrollBuffer();
            operation = "rm";
            select_and_operate_files_by_number(operation, promptFlag, maxDepth, historyPattern, verbose);
            clearScrollBuffer();
            break;
        case '4':
			clearScrollBuffer();
            operation = "mv";
            select_and_operate_files_by_number(operation, promptFlag, maxDepth, historyPattern, verbose);
            clearScrollBuffer();
            break;
        case '5':
			clearScrollBuffer();
            operation = "cp";
            select_and_operate_files_by_number(operation, promptFlag, maxDepth, historyPattern, verbose);
            clearScrollBuffer();
            break;
			}
		}
    }
}


// Function to print submenu2
void submenu2(bool& promptFlag, int& maxDepth, bool& historyPattern, bool& verbose) {
	
	while (true) {
		// Calls prevent_clear_screen and tab completion
		rl_bind_key('\f', prevent_clear_screen_and_tab_completion);
		rl_bind_key('\t', prevent_clear_screen_and_tab_completion);
		
		clearScrollBuffer();
		std::cout << "\033[1;32m+-------------------------+\n";
		std::cout << "\033[1;32m|↵ Convert2ISO             |\n";
		std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|1. CCD2ISO++             |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|2. MDF2ISO++             |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|3. NRG2ISO++             |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\n";
        char* rawInput = readline("\n\001\033[1;94m\002Choose an option:\001\033[0;1m\002 ");

        // Use std::unique_ptr to manage memory for input
		std::unique_ptr<char[], decltype(&std::free)> input(rawInput, &std::free);

        std::string mainInputString(input.get());


        if (!input.get() || std::strlen(input.get()) == 0) {
			break; // Exit the submenu if input is empty or NULL
		}

          std::string submenu_choice(mainInputString);
          std::string operation;
         // Check if the input length is exactly 1
		 if (submenu_choice.empty() || submenu_choice.length() == 1){
         switch (submenu_choice[0]) {
             case '1':
				clearScrollBuffer();
				operation = "bin";
					promptSearchBinImgMdfNrg(operation, promptFlag, maxDepth, historyPattern, verbose);
                clearScrollBuffer();
                break;
             case '2':
				clearScrollBuffer();
				operation = "mdf";
					promptSearchBinImgMdfNrg(operation, promptFlag, maxDepth, historyPattern, verbose);
                clearScrollBuffer();
                break;
             case '3':
				clearScrollBuffer();
				operation = "nrg";
					promptSearchBinImgMdfNrg(operation, promptFlag, maxDepth, historyPattern, verbose);
                clearScrollBuffer();
                break;
			}
		}
	}
}


// Function to print menu
void printMenu() {
    std::cout << "\033[1;32m+-------------------------+\n";
    std::cout << "\033[1;32m|       Menu Options       |\n";
    std::cout << "\033[1;32m+-------------------------+\n";
    std::cout << "\033[1;32m|1. ManageISO             |\n";
    std::cout << "\033[1;32m+-------------------------+\n";
    std::cout << "\033[1;32m|2. Convert2ISO           |\n";
    std::cout << "\033[1;32m+-------------------------+\n";
    std::cout << "\033[1;32m|3. ImportISO             |\n";
    std::cout << "\033[1;32m+-------------------------+\n";
    std::cout << "\033[1;32m|4. Exit                  |\n";
    std::cout << "\033[1;32m+-------------------------+";
    std::cout << "\n";
}


// GENERAL STUFF

// Main verbose print function
void verbosePrint(const std::set<std::string>& primarySet, const std::set<std::string>& secondarySet = {}, const std::set<std::string>& tertiarySet = {}, const std::set<std::string>& quaternarySet = {}, const std::set<std::string>& errorSet = {}, int printType = 0) {
    clearScrollBuffer(); // Assuming this function is defined elsewhere

    // Helper lambda to print a set with optional color and output stream
    auto printSet = [](const std::set<std::string>& set, bool isError = false, bool addNewLineBefore = false) {
        if (!set.empty()) {
            if (addNewLineBefore) {
                std::cout << "\n";
            }
            for (const auto& item : set) {
                if (isError) {
                    // Red color for errors
                    std::cerr << "\n\033[1;91m" << item << "\033[0m\033[1m";
                } else {
                    std::cout << "\n" << item;
                }
            }
        }
    };

    // Determine print behavior based on type
    switch (printType) {
        case 0: // Unmounted
            // Unmounted: primarySet = unmounted files, secondarySet = unmounted errors, errorSet = error messages
            printSet(primarySet);
            printSet(secondarySet, false, !primarySet.empty());
            printSet(errorSet, true, !primarySet.empty() || !secondarySet.empty());
            std::cout << "\n\n";
            break;

        case 1: // Operation
            // Operation: primarySet = operation ISOs, secondarySet = operation errors, errorSet = unique error messages
            printSet(primarySet, false);
            printSet(secondarySet, false, !primarySet.empty());
            printSet(errorSet, false, !primarySet.empty() || !secondarySet.empty());
            std::cout << "\n\n";
            break;

        case 2: // Mounted
            // Mounted: primarySet = mounted files, secondarySet = skipped messages, 
            // tertiarySet = mounted fails, errorSet = unique error messages
            printSet(primarySet);
            printSet(secondarySet, true, !primarySet.empty());
            printSet(tertiarySet, true, !primarySet.empty() || !secondarySet.empty());
            printSet(errorSet, true, !primarySet.empty() || !secondarySet.empty() || !tertiarySet.empty());
            std::cout << "\n\n";
            break;

        case 3: // Conversion
            // Conversion: 
            // primarySet = processed errors
            // secondarySet = success outputs
            // tertiarySet = skipped outputs
            // quaternarySet = failed outputs
            // errorSet = deleted outputs
            std::cout << "\n";
            auto printWithNewline = [](const std::set<std::string>& outs) {
                for (const auto& out : outs) {
                    std::cout << out << "\033[0;1m\n";
                }
                if (!outs.empty()) {
                    std::cout << "\n";
                }
            };

            printWithNewline(secondarySet);   // Success outputs
            printWithNewline(tertiarySet);    // Skipped outputs
            printWithNewline(quaternarySet);  // Failed outputs
            printWithNewline(errorSet);       // Deleted outputs
            printWithNewline(primarySet);     // Processed errors
            break;
    }

    // Continuation prompt
    std::cout << "\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Function to negate original readline bindings
int prevent_clear_screen_and_tab_completion(int, int) {
    // Do nothing and return 0 
    return 0;
}


// Function to clear scrollbuffer
void clearScrollBuffer() {
        std::cout << "\033[3J\033[2J\033[H\033[0m" << std::flush;
}


// Function to flush input buffer
void flushStdin() {
    tcflush(STDIN_FILENO, TCIFLUSH);
}


// Function to disable input during processing
void disableInput() {
    struct termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}


// Function to restore normal input
void restoreInput() {
    struct termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag |= ICANON | ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}


// Function to handle termination signals
void signalHandler(int signum) {

    clearScrollBuffer();
    // Perform cleanup before exiting
    if (lockFileDescriptor != -1) {
        close(lockFileDescriptor);
    }

    exit(signum);
}


// Function to check if a string starts with '0'
bool startsWithZero(const std::string& str) {
    return !str.empty() && str[0] == '0';
}


// Function to check if a string is numeric
bool isNumeric(const std::string& str) {
    return std::all_of(str.begin(), str.end(), [](char c) {
        return std::isdigit(c);
    });
}


// Function to get the sudo invoker ID
void getRealUserId(uid_t& real_uid, gid_t& real_gid, std::string& real_username, std::string& real_groupname,std::set<std::string>& uniqueErrors,std::mutex& Mutex4Low) {

    // Get the real user ID and group ID (of the user who invoked sudo)
    const char* sudo_uid = std::getenv("SUDO_UID");
    const char* sudo_gid = std::getenv("SUDO_GID");

    if (sudo_uid && sudo_gid) {
        try {
            real_uid = static_cast<uid_t>(std::stoul(sudo_uid));
            real_gid = static_cast<gid_t>(std::stoul(sudo_gid));
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(Mutex4Low);
            uniqueErrors.insert("\033[1;91mError parsing SUDO_UID or SUDO_GID environment variables.\033[0;1m");
            return;
        }
    } else {
        // Fallback to current effective user if not running with sudo
        real_uid = geteuid();
        real_gid = getegid();
    }

    // Get real user's name
    struct passwd *pw = getpwuid(real_uid);
    if (pw == nullptr) {
        std::lock_guard<std::mutex> lock(Mutex4Low);
        uniqueErrors.insert("\033[1;91mError getting user information: " + std::string(strerror(errno)) + "\033[0;1m");
        return;
    }
    real_username = pw->pw_name;

    // Get real group name
    struct group *gr = getgrgid(real_gid);
    if (gr == nullptr) {
        std::lock_guard<std::mutex> lock(Mutex4Low);
        uniqueErrors.insert("\033[1;91mError getting group information: " + std::string(strerror(errno)) + "\033[0;1m");
        return;
    }
    real_groupname = gr->gr_name;
}


// General function to tokenize input strings
void tokenizeInput(const std::string& input, std::vector<std::string>& isoFiles, std::set<std::string>& uniqueErrorMessages, std::vector<int>& processedIndices) {
	    std::istringstream iss(input);
	    std::string token;

	    while (iss >> token) {

        // Check if the token starts wit zero and treat it as a non-existent index
        if (startsWithZero(token)) {
			uniqueErrorMessages.emplace("\033[1;91mInvalid index: '0'.\033[0;1m");
			continue;
        }

        // Check if there is more than one hyphen in the token
        if (std::count(token.begin(), token.end(), '-') > 1) {
            uniqueErrorMessages.emplace("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
            continue;
        }

        // Process ranges specified with hyphens
        size_t dashPos = token.find('-');
        if (dashPos != std::string::npos) {
            int start, end;

            try {

                start = std::stoi(token.substr(0, dashPos));
                end = std::stoi(token.substr(dashPos + 1));
            } catch (const std::invalid_argument& e) {
                // Handle the exception for invalid input
                uniqueErrorMessages.emplace("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
                continue;
            } catch (const std::out_of_range& e) {
                // Handle the exception for out-of-range input
                uniqueErrorMessages.emplace("\033[1;91mInvalid range: '" + token + "'.\033[0;1m");
                continue;
            }

            // Check for validity of the specified range
            if ((start < 1 || static_cast<size_t>(start) > isoFiles.size() || end < 1 || static_cast<size_t>(end) > isoFiles.size()) ||
                (start == 0 || end == 0)) {
                uniqueErrorMessages.emplace("\033[1;91mInvalid range: '" + std::to_string(start) + "-" + std::to_string(end) + "'.\033[0;1m");
                continue;
            }

            // Mark indices within the specified range as valid
            int step = (start <= end) ? 1 : -1;
            for (int i = start; ((start <= end) && (i <= end)) || ((start > end) && (i >= end)); i += step) {
                if ((i >= 1) && (i <= static_cast<int>(isoFiles.size())) && std::find(processedIndices.begin(), processedIndices.end(), i) == processedIndices.end()) {
                    processedIndices.push_back(i); // Mark as processed
                } else if ((i < 1) || (i > static_cast<int>(isoFiles.size()))) {
                    uniqueErrorMessages.emplace("\033[1;91mInvalid index '" + std::to_string(i) + "'.\033[0;1m");
                }
            }
        } else if (isNumeric(token)) {
            // Process single numeric indices
            int num = std::stoi(token);
            if (num >= 1 && static_cast<size_t>(num) <= isoFiles.size() && std::find(processedIndices.begin(), processedIndices.end(), num) == processedIndices.end()) {
                processedIndices.push_back(num); // Mark index as processed
            } else if (static_cast<std::vector<std::string>::size_type>(num) > isoFiles.size()) {
                uniqueErrorMessages.emplace("\033[1;91mInvalid index: '" + std::to_string(num) + "'.\033[0;1m");
            }
        } else {
            uniqueErrorMessages.emplace("\033[1;91mInvalid input: '" + token + "'.\033[0;1m");
        }
    }
}


// Function to display progress bar for native operations
void displayProgressBar(const std::atomic<size_t>& completedIsos, const size_t& totalIsos, std::atomic<bool>& isComplete, bool& verbose) {
    const int barWidth = 50;
    bool enterPressed = false;
    auto startTime = std::chrono::high_resolution_clock::now();

    // Set up non-blocking input
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    // Set stdin to non-blocking mode
    int oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

    try {
        while (!isComplete.load() || !enterPressed) {
            // Flush any pending input
            char ch;
            while (read(STDIN_FILENO, &ch, 1) > 0) {
                // Discard any input during progress
            }

            size_t completedValue = completedIsos.load();
            double progress = static_cast<double>(completedValue) / totalIsos;
            int pos = static_cast<int>(barWidth * progress);
            auto currentTime = std::chrono::high_resolution_clock::now();
            auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime);
            double elapsedSeconds = elapsedTime.count() / 1000.0;

            std::cout << "\r[";
            for (int i = 0; i < barWidth; ++i) {
                if (i < pos) std::cout << "=";
                else if (i == pos) std::cout << ">";
                else std::cout << " ";
            }
            std::cout << "] " << std::setw(3) << std::fixed << std::setprecision(1)
                      << (progress * 100.0) << "% (" << completedValue << "/" << totalIsos << ") "
                      << "Time Elapsed: " << std::setprecision(1) << elapsedSeconds << "s";

            if (completedValue == totalIsos && !enterPressed) {
                enterPressed = true;
                
                // Restore terminal to original settings before getting input
                tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
                fcntl(STDIN_FILENO, F_SETFL, oldf);

                std::string confirmation;
                std::cout << "\n\n\033[1;94mDisplay verbose output? (y/n):\033[0;1m ";
                std::getline(std::cin, confirmation);
                
                if (confirmation == "y" || confirmation == "Y") {
                    verbose = true;
                } else {
                    verbose = false;
                }
            } else {
                std::cout.flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Update every 100ms
            }
        }
    }
    catch (...) {
		// Flush any pending input in case of any exceptions
		char ch;
		while (read(STDIN_FILENO, &ch, 1) > 0) {
			// Discard any input during progress
		}
        // Ensure terminal is restored in case of any exceptions
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        fcntl(STDIN_FILENO, F_SETFL, oldf);
        throw;
    }

    // Print a newline after completion
    std::cout << std::endl;

    // Ensure terminal is restored to original settings
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);
    
}


// Function to print all required lists
void printList(const std::vector<std::string>& items, const std::string& listType) {
    static const char* defaultColor = "\033[0m";
    static const char* bold = "\033[1m";
    static const char* reset = "\033[0m";
    static const char* red = "\033[31;1m";
    static const char* green = "\033[32;1m";
    static const char* blueBold = "\033[94;1m";
    static const char* magenta = "\033[95m";
    static const char* magentaBold = "\033[95;1m";
    static const char* orangeBold = "\033[1;38;5;208m";
    
    size_t maxIndex = items.size();
    size_t numDigits = std::to_string(maxIndex).length();

    // Precompute padded index strings
    std::vector<std::string> indexStrings(maxIndex);
    for (size_t i = 0; i < maxIndex; ++i) {
        indexStrings[i] = std::to_string(i + 1);
        indexStrings[i].insert(0, numDigits - indexStrings[i].length(), ' ');
    }

    std::ostringstream output;
    output << "\n"; // Initial newline for visual spacing

    for (size_t i = 0; i < items.size(); ++i) {
        const char* sequenceColor = (i % 2 == 0) ? red : green;
        std::string directory, filename, displayPath;

        if (listType == "ISO_FILES") {
            auto [dir, fname] = extractDirectoryAndFilename(items[i]);
            directory = dir;
            filename = fname;
        } else if (listType == "MOUNTED_ISOS") {
            std::string dirName = items[i];
            size_t lastSlashPos = dirName.find_last_of('/');
            if (lastSlashPos != std::string::npos) {
                dirName = dirName.substr(lastSlashPos + 1);
            }
            size_t underscorePos = dirName.find('_');
            displayPath = ((underscorePos != std::string::npos) ? dirName.substr(underscorePos + 1) : dirName);
        } else if (listType == "IMAGE_FILES") {
            auto [dir, fname] = extractDirectoryAndFilename(items[i]);

            bool isSpecialExtension = false;
            std::string extension = fname;
            size_t dotPos = extension.rfind('.');

            if (dotPos != std::string::npos) {
                extension = extension.substr(dotPos);
                toLowerInPlace(extension);
                isSpecialExtension = (extension == ".bin" || extension == ".img" ||
                                      extension == ".mdf" || extension == ".nrg");
            }

            if (isSpecialExtension) {
                directory = dir;
                filename = fname;
                sequenceColor = orangeBold;
            }
        }

        // Build output based on listType
        if (listType == "ISO_FILES") {
            output << sequenceColor << indexStrings[i] << ". "
                   << defaultColor << bold << directory
                   << defaultColor << bold << "/"
                   << magenta << filename << defaultColor << "\n";
        } else if (listType == "MOUNTED_ISOS") {
            output << sequenceColor << indexStrings[i] << ". "
                   << blueBold << "/mnt/iso_"
                   << magentaBold << displayPath << reset << "\n";
        } else if (listType == "IMAGE_FILES") {
		// Alternate sequence color like in "ISO_FILES"
		const char* sequenceColor = (i % 2 == 0) ? red : green;
    
			if (directory.empty() && filename.empty()) {
				// Standard case
				output << sequenceColor << indexStrings[i] << ". "
				<< reset << bold << items[i] << defaultColor << "\n";
			} else {
				// Special extension case (keep the filename sequence as orange bold)
				output << sequenceColor << indexStrings[i] << ". "
					<< reset << bold << directory << "/"
					<< orangeBold << filename << defaultColor << "\n";
			}
        }
    }

    std::cout << output.str();
}




//	SANITISATION AND STRING STUFF

// Function to escape characters in a string for shell usage
std::string shell_escape(const std::string& s) {
    // Estimate the maximum size of the escaped string
    size_t max_size = s.size() * 4;  // Assuming every character might need escaping
    std::string escaped_string;
    escaped_string.reserve(max_size);

    // Iterate through each character in the input string
    for (char c : s) {
        // Check if the character needs escaping
        if (c == '\'') {
            // If the character is a single quote, replace it with the escaped version
            escaped_string.append("'\\''");
        } else {
            // Otherwise, keep the character unchanged
            escaped_string.push_back(c);
        }
    }

    // Return the escaped string enclosed in single quotes
    return "'" + escaped_string + "'";
}


// Function to extract directory and filename from a given path
std::pair<std::string, std::string> extractDirectoryAndFilename(std::string_view path) {
    // Use string_view for non-modifying operations
    static const std::array<std::pair<std::string_view, std::string_view>, 2> replacements = {{
        {"/home", "~"},
        {"/root", "/R"}
    }};

    // Find last slash efficiently
    auto lastSlashPos = path.find_last_of("/\\");
    if (lastSlashPos == std::string_view::npos) {
        return {"", std::string(path)};
    }

    // Early return for full list mode
    if (toggleFullList) {
        return {std::string(path.substr(0, lastSlashPos)), 
                std::string(path.substr(lastSlashPos + 1))};
    }

    // Check cache first
    auto cacheIt = transformationCache.find(std::string(path));
    if (cacheIt != transformationCache.end()) {
        return {cacheIt->second, std::string(path.substr(lastSlashPos + 1))};
    }

    // Optimize directory shortening
    std::string processedDir;
    processedDir.reserve(path.length() / 2);  // More conservative pre-allocation

    size_t start = 0;
    while (start < lastSlashPos) {
        auto end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos) end = lastSlashPos;

        // More efficient component truncation
        size_t componentLength = end - start;
        size_t truncatePos = std::min({
            componentLength, 
            path.find(' ', start) - start,
            path.find('-', start) - start,
            size_t(28)
        });

        processedDir.append(path.substr(start, truncatePos));
        processedDir.push_back('/');
        start = end + 1;
    }

    if (!processedDir.empty()) {
        processedDir.pop_back();  // Remove trailing slash

        // More efficient replacements using string_view
        for (const auto& [oldDir, newDir] : replacements) {
            size_t pos = 0;
            while ((pos = processedDir.find(oldDir, pos)) != std::string::npos) {
                processedDir.replace(pos, oldDir.length(), newDir);
                pos += newDir.length();
            }
        }
    }

    // Cache the result
    transformationCache[std::string(path)] = processedDir;

    return {processedDir, std::string(path.substr(lastSlashPos + 1))};
}


// Function to load history from readline
void loadHistory(bool& historyPattern) {
    // Only load history from file if it's not already populated in memory
    if (history_length == 0) {
        std::ifstream file;
        if (!historyPattern) {
            file.open(historyFilePath);
        } else {
            file.open(historyPatternFilePath);
        }

        if (file.is_open()) {
            std::string line;
            while (std::getline(file, line)) {
                add_history(line.c_str());
            }
            file.close();
        }
    }
}


// Function to save history from readline
void saveHistory(bool& historyPattern) {
    std::ofstream historyFile;
    
    // Choose file path based on historyPattern flag
    std::string targetFilePath = !historyPattern ? 
        historyFilePath : historyPatternFilePath;
    
    // Open file in truncate mode
    historyFile.open(targetFilePath, std::ios::out | std::ios::trunc);
    
    if (!historyFile.is_open()) {
        return;
    }

    HIST_ENTRY **histList = history_list();
    if (!histList) return;

    std::unordered_map<std::string, size_t> lineIndices;
    std::vector<std::string> uniqueLines;

    // Iterate through history entries
    for (int i = 0; histList[i]; i++) {
        std::string line(histList[i]->line);
        if (line.empty()) continue;

        // Remove existing duplicate if present, then add to end
        auto it = lineIndices.find(line);
        if (it != lineIndices.end()) {
            uniqueLines.erase(uniqueLines.begin() + it->second);
            lineIndices.erase(it);
        }

        // Add new line to end and update index
        lineIndices[line] = uniqueLines.size();
        uniqueLines.push_back(line);
    }

    // Determine max lines based on pattern flag
    size_t maxLines = !historyPattern ? 
        MAX_HISTORY_LINES : MAX_HISTORY_PATTERN_LINES;

    // Trim excess lines if needed
    if (uniqueLines.size() > maxLines) {
        uniqueLines.erase(
            uniqueLines.begin(), 
            uniqueLines.begin() + (uniqueLines.size() - maxLines)
        );
    }

    // Write unique lines to file
    for (const auto& line : uniqueLines) {
        historyFile << line << std::endl;
    }

    historyFile.close();
}
