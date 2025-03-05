// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../display.h"


// Get max available CPU cores for global use, fallback is 2 cores
unsigned int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;

// Global mutex to protect the verbose sets
std::mutex globalSetsMutex;

const std::string configPath = std::string(getenv("HOME")) + "/.config/isocmd/config/config";

// Global variables for cleanup
int lockFileDescriptor = -1;

// Global falg to track cancellation
std::atomic<bool> g_operationCancelled{false};


// Default Display config options for lists
namespace displayConfig {
    bool toggleFullListMount = false;
    bool toggleFullListUmount = true;
    bool toggleFullListCpMvRm = false;
    bool toggleFullListWrite = false;
    bool toggleFullListConversions = false;
}


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
	// Atomic flag for auto-update
	std::atomic<bool> isImportRunning;
	// Global atomic flag for auto-update message state
	std::atomic<bool> messageActive{false};
	
	// For indicating if location is int main
	std::atomic<bool> isAtMain{true};
	
	// For indicating if ISO list is visible
	std::atomic<bool> isAtISOList{false};
	
	// For indicating if auto-update has run
	std::atomic<bool> updateHasRun{false};
	
	// Variable to track if a new .iso file is found after search
	std::atomic<bool> newISOFound{false};
	
	setupReadlineToIgnoreCtrlC();
	
	if (argc == 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-v")) {
        printVersionNumber("5.7.9");
        return 0;
    }
    // Readline use semicolon as delimiter
    rl_completer_word_break_characters =";";

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
    signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
    signal(SIGTERM, signalHandler); // Handle termination signals

    bool exitProgram = false;
    
    // Automatic ISO  cache Import
    bool search = false;      
    
    std::map<std::string, std::string> config = readUserConfigLists(configPath);
    
    std::string choice;
    isImportRunning.store(false);
    // Open the history file for automatic ISO cache imports
    std::ifstream file(historyFilePath);
	search = readUserConfigUpdates(configPath); 
    
	if (search) {
		isImportRunning.store(true);
		std::thread([maxDepth, &isImportRunning, &newISOFound]() {
			backgroundCacheImport(maxDepth, isImportRunning, newISOFound);
		}).detach();
		updateHasRun.store(true);
	}
	
	// End of automatic cache import

    while (!exitProgram) {
		// Calls prevent_clear_screen and tab completion
		rl_bind_key('\f', prevent_readline_keybindings);
		rl_bind_key('\t', prevent_readline_keybindings);
		
		g_operationCancelled.store(false);
		
		// For indicating if location is int main
		isAtMain.store(true);
		isAtISOList.store(false);

		globalIsoFileList.reserve(100);
        clearScrollBuffer();
        print_ascii();
        
        // Check if auto update is running or has no paths to process
        static bool messagePrinted = false;
        namespace fs = std::filesystem;
        if (search && !isHistoryFileEmpty(historyFilePath) && isImportRunning.load()) {
            std::cout << "\033[2m[Auto-update: running in the background...]\033[0m\n";
            messageActive.store(true);

            // Launch a thread to poll for clear message every 1 second
            std::thread(clearMessageAfterTimeout, 1, std::ref(isAtMain), std::ref(isImportRunning), std::ref(messageActive)).detach();
		} else if ((search && !messagePrinted) && (isHistoryFileEmpty(historyFilePath) || !fs::is_regular_file(historyFilePath))) {
			std::cout << "\033[2m[Auto-update: no stored folder paths to scan...]\033[0m\n";
			messagePrinted = true;
			messageActive.store(true);
			// clear message after 4 seconds
			std::thread(clearMessageAfterTimeout, 4, std::ref(isAtMain), std::ref(isImportRunning), std::ref(messageActive)).detach();
		}
        
        // Display the main menu options
        printMenu();

        // Clear history
        clear_history();

        // Prompt for the main menu choice
        char* rawInput = readline("\n\001\033[1;94m\002Choose an option:\001\033[0;1m\002 ");
        std::unique_ptr<char[], decltype(&std::free)> input(rawInput, &std::free);

        // Check for EOF (Ctrl+D) or NULL input before processing
        if (!input.get()) {
            break; // Exit the loop on EOF
        }

        std::string mainInputString(input.get());
        std::string choice(mainInputString);
        std::string initialDir = "";

        if (choice == "1") {
			isAtMain.store(false);
			isAtISOList.store(false);
            submenu1(maxDepth, historyPattern, verbose, updateHasRun, isAtISOList, isImportRunning, newISOFound);
        } else {
            // Check if the input length is exactly 1
            if (choice.length() == 1) {
                switch (choice[0]) {
                    case '2':
						isAtMain.store(false);
						isAtISOList.store(false);
                        submenu2(promptFlag, maxDepth, historyPattern, verbose, newISOFound);
                        break;
                    case '3':
						isAtMain.store(false);
						isAtISOList.store(false);
                        manualRefreshCache(initialDir, promptFlag, maxDepth, historyPattern, newISOFound);
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
void submenu1(int& maxDepth, bool& historyPattern, bool& verbose, std::atomic<bool>& updateHasRun, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound) {
	
    while (true) {
		// Calls prevent_clear_screen and tab completion
		rl_bind_key('\f', prevent_readline_keybindings);
		rl_bind_key('\t', prevent_readline_keybindings);
		
		isAtISOList.store(false);
		
        clearScrollBuffer();
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|↵ Manage ISO              |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|1. Mount                 |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|2. Umount                |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|3. Delete                |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|4. Move                  |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|5. Copy                  |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|6. Write                 |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\n";
        char* rawInput = readline("\001\033[1;94m\002Choose an option:\001\033[0;1m\002 ");

        // Use std::unique_ptr to manage memory for input
		std::unique_ptr<char[], decltype(&std::free)> input(rawInput, &std::free);

        // Check for EOF (Ctrl+D) or NULL input before processing
        if (!input.get()) {
            break; // Exit the loop on EOF
        }

        std::string mainInputString(input.get());
        std::string choice(mainInputString);

        if (!input.get() || std::strlen(input.get()) == 0) {
			break; // Exit the submenu if input is empty or NULL
		}

          std::string submenu_choice(mainInputString);
         // Check if the input length is exactly 1
        if (submenu_choice.empty() || submenu_choice.length() == 1) {
		switch (submenu_choice[0]) {
        case '1':
			clearScrollBuffer();
            selectForIsoFiles("mount", historyPattern, maxDepth, verbose, updateHasRun, isAtISOList, isImportRunning, newISOFound);
            clearScrollBuffer();
            break;
        case '2':
			clearScrollBuffer();
            selectForIsoFiles("umount", historyPattern, maxDepth, verbose, updateHasRun, isAtISOList, isImportRunning, newISOFound);
            clearScrollBuffer();
            break;
        case '3':
			clearScrollBuffer();
            selectForIsoFiles("rm", historyPattern, maxDepth, verbose, updateHasRun, isAtISOList, isImportRunning, newISOFound);
            clearScrollBuffer();
            break;
        case '4':
			clearScrollBuffer();
            selectForIsoFiles("mv", historyPattern, maxDepth, verbose, updateHasRun, isAtISOList, isImportRunning, newISOFound);

            clearScrollBuffer();
            break;
        case '5':
			clearScrollBuffer();
            selectForIsoFiles("cp", historyPattern, maxDepth, verbose, updateHasRun, isAtISOList, isImportRunning, newISOFound);
            clearScrollBuffer();
            break;
        case '6':
			clearScrollBuffer();
            selectForIsoFiles("write", historyPattern, maxDepth, verbose, updateHasRun, isAtISOList, isImportRunning, newISOFound);
            clearScrollBuffer();
            break;
			}
		}
    }
}


// Function to print submenu2
void submenu2(bool& promptFlag, int& maxDepth, bool& historyPattern, bool& verbose, std::atomic<bool>& newISOFound) {
	
	while (true) {
		// Calls prevent_clear_screen and tab completion
		rl_bind_key('\f', prevent_readline_keybindings);
		rl_bind_key('\t', prevent_readline_keybindings);
		
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
        char* rawInput = readline("\001\033[1;94m\002Choose an option:\001\033[0;1m\002 ");

        // Use std::unique_ptr to manage memory for input
		std::unique_ptr<char[], decltype(&std::free)> input(rawInput, &std::free);

        // Check for EOF (Ctrl+D) or NULL input before processing
        if (!input.get()) {
            break; // Exit the loop on EOF
        }

        std::string mainInputString(input.get());
        std::string choice(mainInputString);


        if (!input.get() || std::strlen(input.get()) == 0) {
			break; // Exit the submenu if input is empty or NULL
		}

          std::string submenu_choice(mainInputString);
          std::string operation;
         // Check if the input length is exactly 1
		 if (submenu_choice.empty() || submenu_choice.length() == 1){
         switch (submenu_choice[0]) {
             case '1':
				operation = "bin";
					promptSearchBinImgMdfNrg(operation, promptFlag, maxDepth, historyPattern, verbose, newISOFound);
                clearScrollBuffer();
                break;
             case '2':
				operation = "mdf";
					promptSearchBinImgMdfNrg(operation, promptFlag, maxDepth, historyPattern, verbose, newISOFound);
                clearScrollBuffer();
                break;
             case '3':
				operation = "nrg";
					promptSearchBinImgMdfNrg(operation, promptFlag, maxDepth, historyPattern, verbose, newISOFound);
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

// function to read and map config file
std::map<std::string, std::string> readConfig(const std::string& configPath) {
    std::map<std::string, std::string> config;
    std::ifstream inFile(configPath);
    
    auto trim = [](std::string str) {
        str.erase(0, str.find_first_not_of(" "));
        str.erase(str.find_last_not_of(" ") + 1);
        return str;
    };
    
    if (inFile.is_open()) {
        std::string line;
        while (std::getline(inFile, line)) {
            size_t equalPos = line.find('=');
            if (equalPos != std::string::npos) {
                std::string key = line.substr(0, equalPos);
                std::string value = line.substr(equalPos + 1);
                config[trim(key)] = trim(value);
            }
        }
        inFile.close();
    }
    
    return config;
}

// Function to get AutomaticImportConfig status
bool readUserConfigUpdates(const std::string& filePath) {
    std::ifstream inFile(filePath);
    if (!inFile) {
        return false; // Default to false if file cannot be opened
    }

    std::string line;
    while (std::getline(inFile, line)) {
        // Remove leading and trailing whitespace from the line
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);

        // Check if the line starts with "auto_ISO_updates"
        if (line.find("auto_update") == 0) {
            // Find the position of the '=' character
            size_t equalsPos = line.find('=');
            if (equalsPos == std::string::npos) {
                return false; // No '=' found, invalid format
            }

            // Extract the value part (after '=')
            std::string valueStr = line.substr(equalsPos + 1);
            // Remove leading and trailing whitespace from the value
            valueStr.erase(0, valueStr.find_first_not_of(" \t"));
            valueStr.erase(valueStr.find_last_not_of(" \t") + 1);

            // Convert the value to an integer
            try {
                int userChoice = std::stoi(valueStr);
                // Check if the value is 0 or 1
                if (userChoice == 0 || userChoice == 1) {
                    return (userChoice == 1); // Return true if 1, false if 0
                } else {
                    return false; // Invalid value (not 0 or 1)
                }
            } catch (const std::invalid_argument&) {
                return false; // Value is not a valid integer
            } catch (const std::out_of_range&) {
                return false; // Value is out of range for an integer
            }
        }
    }

    return false; // Key "auto_ISO_updates" not found in the file
}


// Function to set list mode based on config file
std::map<std::string, std::string> readUserConfigLists(const std::string& filePath) {
    std::map<std::string, std::string> configMap;
    std::ifstream inFile(filePath);

    // Default values for required keys
    std::map<std::string, std::string> defaultConfig = {
        {"mount_list", "compact"},
        {"umount_list", "full"}, // Default for umount is long
        {"cp_mv_rm_list", "compact"},
        {"write_list", "compact"},
        {"conversion_lists", "compact"}
    };

    // If the file cannot be opened, write the default configuration and return it
    if (!inFile) {
        std::ofstream outFile(filePath);
        if (!outFile) {
            return defaultConfig; // Return default config if file creation fails
        }

        // Write default configuration to the file
        for (const auto& pair : defaultConfig) {
            outFile << pair.first << " = " << pair.second << "\n";
        }

        return defaultConfig;
    }

    // Read the existing configuration file
    std::string line;
    while (std::getline(inFile, line)) {
        // Remove leading and trailing whitespace from the line
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);

        // Skip empty lines or comments (lines starting with '#')
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Find the position of the '=' character
        size_t equalsPos = line.find('=');
        if (equalsPos == std::string::npos) {
            continue; // Skip lines without '='
        }

        // Extract the key and value parts
        std::string key = line.substr(0, equalsPos);
        std::string valueStr = line.substr(equalsPos + 1);

        // Remove leading and trailing whitespaces from both key and value
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        valueStr.erase(0, valueStr.find_first_not_of(" \t"));
        valueStr.erase(valueStr.find_last_not_of(" \t") + 1);

        // Check if the key is one of the required keys
        if (defaultConfig.find(key) != defaultConfig.end()) {
            // Check if the value is "short" or "long"
            if (valueStr == "compact" || valueStr == "full") {
                configMap[key] = valueStr;
            } else {
                configMap[key] = defaultConfig[key]; // Use default value for invalid value
            }
        } else if (key == "auto_update") {
            // Handle auto_update separately if needed
            configMap[key] = valueStr;
        }
    }

    // Add missing keys with default values
    for (const auto& pair : defaultConfig) {
        if (configMap.find(pair.first) == configMap.end()) {
            configMap[pair.first] = pair.second; // Add default key-value pair
        }
    }

    // Write the updated configuration back to the file (if any keys were missing)
    if (configMap.size() > defaultConfig.size()) {
        std::ofstream outFile(filePath);
        if (outFile) {
            for (const auto& pair : configMap) {
                outFile << pair.first << " = " << pair.second << "\n";
            }
        }
    }

    // Set the boolean values based on the configMap
    displayConfig::toggleFullListMount = (configMap["mount_list"] == "full");
    displayConfig::toggleFullListUmount = (configMap["umount_list"] == "full");
    displayConfig::toggleFullListCpMvRm = (configMap["cp_mv_rm_list"] == "full");
    displayConfig::toggleFullListWrite = (configMap["write_list"] == "full");
    displayConfig::toggleFullListConversions = (configMap["conversion_lists"] == "full");

    return configMap;
}


// Function to clear the message after a timeout
void clearMessageAfterTimeout(int timeoutSeconds, std::atomic<bool>& isAtMain, std::atomic<bool>& isImportRunning, std::atomic<bool>& messageActive) {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(timeoutSeconds));
        
        if (!isImportRunning.load()) {
            if (messageActive.load() && isAtMain.load()) {
                clearScrollBuffer();
                
                print_ascii();
                
                printMenu();
                std::cout << "\n";
                rl_on_new_line(); 
                rl_redisplay();

                messageActive.store(false);
            }
            break; // Exit the loop once the message is cleared
        }
    }
}


// Function to negate original readline bindings
int prevent_readline_keybindings(int, int) {
    // Do nothing and return 0 
    return 0;
}


// Function to clear scroll buffer in addition to clearing screen with ctrl+l
int clear_screen_and_buffer(int, int) {
    // Clear scroll buffer and screen (works in most terminals)
    clearScrollBuffer();
    fflush(stdout);
    // Force readline to redisplay with the current prompt
    rl_forced_update_display();
    return 0;
}


// Function to clear scrollbuffer
void clearScrollBuffer() {
        std::cout << "\033[3J\033[2J\033[H\033[0m" << std::flush;
}


// Function to disable (Ctrl+D)
void disable_ctrl_d() {
    struct termios term;
    
    // Get current terminal attributes
    tcgetattr(STDIN_FILENO, &term);
    
    // Disable EOF (Ctrl+D) processing
    term.c_cc[VEOF] = _POSIX_VDISABLE;
    
    // Apply the modified settings
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}


// Function to specifically re-enable Ctrl+D
void enable_ctrl_d() {
    struct termios term;
    
    // Get current terminal attributes
    tcgetattr(STDIN_FILENO, &term);
    
    // Re-enable EOF (Ctrl+D) - typically ASCII 4 (EOT)
    term.c_cc[VEOF] = 4;  // Default value for most systems
    
    // Apply the modified settings
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
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


// Function to ignore Ctrl+C
void setupReadlineToIgnoreCtrlC() {
    // Prevent readline from catching/interrupting signals
    rl_catch_signals = 0;

    // Configure SIGINT (Ctrl+C) to be ignored
    struct sigaction sa_ignore;
    sa_ignore.sa_handler = SIG_IGN;   // Ignore signal
    sigemptyset(&sa_ignore.sa_mask);  // Clear signal mask
    sa_ignore.sa_flags = 0;           // No special flags
    sigaction(SIGINT, &sa_ignore, nullptr);
}


// Signal handler for SIGINT (Ctrl+C)
void signalHandlerCancellations(int signal) {
    if (signal == SIGINT) {
        g_operationCancelled = true;
    }
}

// Setup signal handling
void setupSignalHandlerCancellations() {
    struct sigaction sa;
    sa.sa_handler = signalHandlerCancellations;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
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


// Function to check if a file already exists
bool fileExists(const std::string& fullPath) {
        return std::filesystem::exists(fullPath);
}


// Function to check if a string is numeric
bool isNumeric(const std::string& str) {
    return std::all_of(str.begin(), str.end(), [](char c) {
        return std::isdigit(c);
    });
}


// Function to check if directory exists
bool directoryExists(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) {
        return false;
    }
    return info.st_mode & S_IFDIR; // Check if it's a directory
}


// Function to check if directory is empty
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


// Compare two strings in natural order, case-insensitively.
int naturalCompare(const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(a[i]) && std::isdigit(b[j])) {
            // Skip leading zeros and compute lengths
            size_t start_a = i, start_b = j;
            while (start_a < a.size() && a[start_a] == '0') start_a++;
            while (start_b < b.size() && b[start_b] == '0') start_b++;
            
            // Compute total digit lengths
            size_t len_a = 0, len_b = 0;
            while (i + len_a < a.size() && std::isdigit(a[i + len_a])) len_a++;
            while (j + len_b < b.size() && std::isdigit(b[j + len_b])) len_b++;
            
            // Non-zero lengths via subtraction (optimization)
            size_t nz_len_a = len_a - (start_a - i);
            size_t nz_len_b = len_b - (start_b - j);
            
            // Compare non-zero lengths
            if (nz_len_a != nz_len_b)
                return (nz_len_a < nz_len_b) ? -1 : 1;
            
            // Compare digit by digit if lengths match
            for (size_t k = 0; k < nz_len_a; ++k) {
                char ca = (start_a + k < a.size()) ? a[start_a + k] : '0';
                char cb = (start_b + k < b.size()) ? b[start_b + k] : '0';
                if (ca != cb) return (ca < cb) ? -1 : 1;
            }
            
            // Compare leading zeros if digits are equal
            size_t zeros_a = start_a - i;
            size_t zeros_b = start_b - j;
            if (zeros_a != zeros_b)
                return (zeros_a < zeros_b) ? -1 : 1;
            
            i += len_a;
            j += len_b;
        } else {
            // Case-insensitive compare for non-digits
            char ca = std::tolower(a[i]), cb = std::tolower(b[j]);
            if (ca != cb) return (ca < cb) ? -1 : 1;
            ++i; ++j;
        }
    }
    return (i < a.size()) ? 1 : (j < b.size()) ? -1 : 0;
}


// Sort files using a natural order, case-insensitive comparator.
void sortFilesCaseInsensitive(std::vector<std::string>& files) {
    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
        return naturalCompare(a, b) < 0;
    });
}


// Function to get the sudo invoker ID
void getRealUserId(uid_t& real_uid, gid_t& real_gid, std::string& real_username, std::string& real_groupname) {
    // Reset output parameters to prevent any uninitialized memory issues
    real_uid = static_cast<uid_t>(-1);
    real_gid = static_cast<gid_t>(-1);
    real_username.clear();
    real_groupname.clear();

    // Get the real user ID and group ID (of the user who invoked sudo)
    const char* sudo_uid = std::getenv("SUDO_UID");
    const char* sudo_gid = std::getenv("SUDO_GID");

    if (sudo_uid && sudo_gid) {
        char* endptr;
        errno = 0;
        unsigned long uid_val = std::strtoul(sudo_uid, &endptr, 10);
        
        // Check for conversion errors
        if (errno != 0 || *endptr != '\0' || endptr == sudo_uid) {
            // Fallback to current effective user
            real_uid = geteuid();
            real_gid = getegid();
        } else {
            real_uid = static_cast<uid_t>(uid_val);
            
            errno = 0;
            unsigned long gid_val = std::strtoul(sudo_gid, &endptr, 10);
            
            // Check for conversion errors
            if (errno != 0 || *endptr != '\0' || endptr == sudo_gid) {
                // Fallback to current effective group
                real_gid = getegid();
            } else {
                real_gid = static_cast<gid_t>(gid_val);
            }
        }
    } else {
        // Fallback to current effective user if not running with sudo
        real_uid = geteuid();
        real_gid = getegid();
    }

    // Get real user's name (with more robust error handling)
    struct passwd pwd_result;
    struct passwd *pwd = nullptr;
    char buffer[1024];  // Recommended buffer size for getpwuid_r

    int ret = getpwuid_r(real_uid, &pwd_result, buffer, sizeof(buffer), &pwd);
    if (ret == 0 && pwd != nullptr) {
        real_username = pwd->pw_name ? pwd->pw_name : "";
    } else {
        // Fallback if unable to retrieve username
        real_username = "unknown";
    }

    // Get real group name (with more robust error handling)
    struct group grp_result;
    struct group *grp = nullptr;
    char grp_buffer[1024];  // Recommended buffer size for getgrgid_r

    ret = getgrgid_r(real_gid, &grp_result, grp_buffer, sizeof(grp_buffer), &grp);
    if (ret == 0 && grp != nullptr) {
        real_groupname = grp->gr_name ? grp->gr_name : "";
    } else {
        // Fallback if unable to retrieve group name
        real_groupname = "unknown";
    }
}
