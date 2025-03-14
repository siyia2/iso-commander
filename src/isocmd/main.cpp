// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../display.h"


// Get max available CPU cores for global use, fallback is 2 cores
unsigned int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;

// Global mutex to protect the verbose sets
std::mutex globalSetsMutex;

const std::string configPath = std::string(getenv("HOME")) + "/.config/isocmd/config";

// Global variables for cleanup
int lockFileDescriptor = -1;

// Global falg to track cancellation
std::atomic<bool> g_operationCancelled{false};

// Pagination variables
size_t currentPage = 0;

// Default max entries per listed page
size_t ITEMS_PER_PAGE = 25;

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
        printVersionNumber("5.9.0");
        return 0;
    }
    // Readline use semicolon as delimiter
    rl_completer_word_break_characters =";";
    
    // Readline do not repaint prompt on list completion
    rl_completion_display_matches_hook = customListingsFunction;


    const char* lockFile = "/tmp/isocmd.lock";

    lockFileDescriptor = open(lockFile, O_CREAT | O_RDWR, 0666);

    struct flock fl;
    fl.l_type = F_WRLCK;  // Write lock
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;  // Lock the whole file

    if (fcntl(lockFileDescriptor, F_SETLK, &fl) == -1) {
        std::cerr << "\033[93mAnother instance of isocmd is already running. If not run \'rm /tmp/isocmd.lock\'.\n\033[0m";
        close(lockFileDescriptor);
        return 1;
    }

    // Register signal handlers
    signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
    signal(SIGTERM, signalHandler); // Handle termination signals

    bool exitProgram = false;
    
    // Automatic ISO Import
    bool search = false;      
    
    std::map<std::string, std::string> config = readUserConfigLists(configPath);
    
    std::string choice;
    isImportRunning.store(false);
    // Open the history file for automatic ISO import
    std::ifstream file(historyFilePath);
	search = readUserConfigUpdates(configPath); 
    
	if (search) {
		isImportRunning.store(true);
		std::thread([&isImportRunning, &newISOFound]() {
			backgroundDatabaseImport(isImportRunning, newISOFound);
		}).detach();
		updateHasRun.store(true);
	}
	
	// End of automatic ISO import
	
	// Set max entries per page in lists
	paginationSet(configPath);
	
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
        if (search && !isHistoryFileEmpty(historyFilePath) && isImportRunning.load()) {
            std::cout << "\033[2m[Auto-Update: running in the background...]\033[0m\n";
            messageActive.store(true);

            // Launch a thread to poll for clear message every 1 second
            std::thread(clearMessageAfterTimeout, 1, std::ref(isAtMain), std::ref(isImportRunning), std::ref(messageActive)).detach();
		} else if ((search && !messagePrinted) && (isHistoryFileEmpty(historyFilePath) || !fs::is_regular_file(historyFilePath))) {
			std::cout << "\033[2m[Auto-Update: no stored folder paths to scan...]\033[0m\n";
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
            submenu1(updateHasRun, isAtISOList, isImportRunning, newISOFound);
        } else {	
			bool promptFlag; // Flag for enabing interactive refresh prompt
			bool filterHistory; // Filter history toggle
			int maxDepth; // Traverse depth variable

            // Check if the input length is exactly 1
            if (choice.length() == 1) {
                switch (choice[0]) {
                    case '2':
						isAtMain.store(false);
						isAtISOList.store(false);
                        submenu2(newISOFound);
                        break;
                    case '3':
						isAtMain.store(false);
						isAtISOList.store(false);
						promptFlag = true; // Enable prompt
						filterHistory = false; // Disable filter hsitory
						maxDepth = -1; // Traverse at max depth
                        manualRefreshForDatabase(initialDir, promptFlag, maxDepth, filterHistory, newISOFound);
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
void submenu1(std::atomic<bool>& updateHasRun, std::atomic<bool>& isAtISOList, std::atomic<bool>& isImportRunning, std::atomic<bool>& newISOFound) {
	
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
            selectForIsoFiles("mount", updateHasRun, isAtISOList, isImportRunning, newISOFound);
            clearScrollBuffer();
            break;
        case '2':
			clearScrollBuffer();
            selectForIsoFiles("umount", updateHasRun, isAtISOList, isImportRunning, newISOFound);
            clearScrollBuffer();
            break;
        case '3':
			clearScrollBuffer();
            selectForIsoFiles("rm", updateHasRun, isAtISOList, isImportRunning, newISOFound);
            clearScrollBuffer();
            break;
        case '4':
			clearScrollBuffer();
            selectForIsoFiles("mv", updateHasRun, isAtISOList, isImportRunning, newISOFound);

            clearScrollBuffer();
            break;
        case '5':
			clearScrollBuffer();
            selectForIsoFiles("cp", updateHasRun, isAtISOList, isImportRunning, newISOFound);
            clearScrollBuffer();
            break;
        case '6':
			clearScrollBuffer();
            selectForIsoFiles("write", updateHasRun, isAtISOList, isImportRunning, newISOFound);
            clearScrollBuffer();
            break;
			}
		}
    }
}


// Function to print submenu2
void submenu2(std::atomic<bool>& newISOFound) {
	
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
					promptSearchBinImgMdfNrg(operation, newISOFound);
                clearScrollBuffer();
                break;
             case '2':
				operation = "mdf";
					promptSearchBinImgMdfNrg(operation, newISOFound);
                clearScrollBuffer();
                break;
             case '3':
				operation = "nrg";
					promptSearchBinImgMdfNrg(operation, newISOFound);
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

// Function to read and map config file
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
    std::map<std::string, std::string> configMap;
    std::ifstream inFile(filePath);
    // Default ordered settings
    std::vector<std::pair<std::string, std::string>> orderedDefaults = {
        {"auto_update", "off"},
        {"pagination", "25"},
        {"mount_list", "compact"},
        {"umount_list", "full"},
        {"cp_mv_rm_list", "compact"},
        {"write_list", "compact"},
        {"conversion_lists", "compact"}
    };
    // If file cannot be opened, return false
    if (!inFile) return false;
    // Read file content
    std::string line;
    while (std::getline(inFile, line)) {
        // Trim whitespace
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);
        if (line.empty() || line[0] == '#') continue;
        // Find '=' character
        size_t equalsPos = line.find('=');
        if (equalsPos == std::string::npos) continue;
        // Extract key and value
        std::string key = line.substr(0, equalsPos);
        std::string valueStr = line.substr(equalsPos + 1);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        valueStr.erase(0, valueStr.find_first_not_of(" \t"));
        valueStr.erase(valueStr.find_last_not_of(" \t") + 1);
        // Store only recognized keys
        for (const auto& pair : orderedDefaults) {
            if (key == pair.first) {
                configMap[key] = valueStr;
                break;
            }
        }
    }
    inFile.close();
    // Ensure default order and missing keys
    bool needsUpdate = false;
    for (const auto& pair : orderedDefaults) {
        if (configMap.find(pair.first) == configMap.end()) {
            configMap[pair.first] = pair.second;
            needsUpdate = true;
        }
    }
    // Update the file if missing keys were added
    if (needsUpdate) {
        std::ofstream outFile(filePath);
        if (outFile) {
            for (const auto& pair : orderedDefaults) {
                outFile << pair.first << " = " << configMap[pair.first] << "\n";
            }
        }
    }
    // Return auto_update setting
    return (configMap["auto_update"] == "on");
}


// Function to set ITEMS_PER_PAGE
bool paginationSet(const std::string& filePath) {
    std::map<std::string, std::string> configMap;
    std::ifstream inFile(filePath);

    if (!inFile) return false;

    std::string line;
    while (std::getline(inFile, line)) {
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);

        if (line.empty() || line[0] == '#') continue;

        size_t equalsPos = line.find('=');
        if (equalsPos == std::string::npos) continue;

        std::string key = line.substr(0, equalsPos);
        std::string valueStr = line.substr(equalsPos + 1);

        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        valueStr.erase(0, valueStr.find_first_not_of(" \t"));
        valueStr.erase(valueStr.find_last_not_of(" \t") + 1);

        if (key == "pagination") {
            try {
                ITEMS_PER_PAGE = std::stoi(valueStr);
                return true;
            } catch (...) {
                return false;
            }
        }
    }
    return false;
}


// Function to set list mode based on config file
std::map<std::string, std::string> readUserConfigLists(const std::string& filePath) {
    std::map<std::string, std::string> configMap;

    // Default values with a fixed order
    std::vector<std::pair<std::string, std::string>> orderedDefaults = {
        {"auto_update", "off"},
        {"pagination", "25"},
        {"mount_list", "compact"},
        {"umount_list", "full"},
        {"cp_mv_rm_list", "compact"},
        {"write_list", "compact"},
        {"conversion_lists", "compact"}
    };

    // Ensure the parent directory exists
    fs::path configPath(filePath);
    if (!fs::exists(configPath.parent_path()) && !configPath.parent_path().empty()) {
        fs::create_directories(configPath.parent_path());
    }

    std::ifstream inFile(filePath);

    // If the file cannot be opened, create and write defaults
    if (!inFile) {
        std::ofstream outFile(filePath);
        if (!outFile) {
            return std::map<std::string, std::string>(orderedDefaults.begin(), orderedDefaults.end());
        }

        for (const auto& pair : orderedDefaults) {
            outFile << pair.first << " = " << pair.second << "\n";
        }

        return std::map<std::string, std::string>(orderedDefaults.begin(), orderedDefaults.end());
    }

    // Read existing config
    std::string line;
    while (std::getline(inFile, line)) {
        // Remove leading/trailing whitespace
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);

        // Skip empty lines or comments
        if (line.empty() || line[0] == '#') {
            continue;
        }

        size_t equalsPos = line.find('=');
        if (equalsPos == std::string::npos) {
            continue; // Skip malformed lines
        }

        std::string key = line.substr(0, equalsPos);
        std::string valueStr = line.substr(equalsPos + 1);

        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        valueStr.erase(0, valueStr.find_first_not_of(" \t"));
        valueStr.erase(valueStr.find_last_not_of(" \t") + 1);

        for (const auto& pair : orderedDefaults) {
            if (key == pair.first) {
                configMap[key] = valueStr;
                break;
            }
        }
    }

    inFile.close();

    // Add missing keys with default values
    bool needsUpdate = false;
    for (const auto& pair : orderedDefaults) {
        if (configMap.find(pair.first) == configMap.end()) {
            configMap[pair.first] = pair.second;
            needsUpdate = true;
        }
    }

    // Update the file if needed
    if (needsUpdate) {
		std::ofstream outFile(filePath);
		if (outFile) {
			for (const auto& pair : orderedDefaults) {
				outFile << pair.first << " = " << configMap[pair.first] << "\n";
			}
		}
	}

    // Set boolean flags based on configMap
    displayConfig::toggleFullListMount = (configMap["mount_list"] == "full");
    displayConfig::toggleFullListUmount = (configMap["umount_list"] == "full");
    displayConfig::toggleFullListCpMvRm = (configMap["cp_mv_rm_list"] == "full");
    displayConfig::toggleFullListWrite = (configMap["write_list"] == "full");
    displayConfig::toggleFullListConversions = (configMap["conversion_lists"] == "full");

    return configMap;
}


// Function to write numer of entries per page for pagination
void updatePagination(const std::string& inputSearch, const std::string& configPath) {
    signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
    disable_ctrl_d();

    // Create directory if it doesn't exist
    std::filesystem::path dirPath = std::filesystem::path(configPath).parent_path();
    if (!std::filesystem::exists(dirPath)) {
        if (!std::filesystem::create_directories(dirPath)) {
            std::cerr << "\n\033[1;91mFailed to create directory: \033[1;93m'" 
                      << dirPath.string() << "\033[1;91m'.\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return;
        }
    }

    int paginationValue = 0;
    std::string paginationValueStr;
    
    try {
        // Extract the pagination value
        paginationValueStr = inputSearch.substr(12);
        // Try to parse the pagination value
        paginationValue = std::stoi(paginationValueStr);
    }
    catch (const std::invalid_argument&) {
        std::cerr << "\n\033[1;91mInvalid pagination value: '\033[1;93m" 
                  << paginationValueStr << "\033[1;91m' is not a valid number.\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }

    // Read current configuration
    std::map<std::string, std::string> config = readConfig(configPath);
    config["pagination"] = std::to_string(paginationValue);

    std::vector<std::pair<std::string, std::string>> orderedDefaults = {
        {"auto_update", config.count("auto_update") ? config["auto_update"] : "off"},
        {"pagination", std::to_string(paginationValue)},
        {"mount_list", config.count("mount_list") ? config["mount_list"] : "compact"},
        {"umount_list", config.count("umount_list") ? config["umount_list"] : "full"},
        {"cp_mv_rm_list", config.count("cp_mv_rm_list") ? config["cp_mv_rm_list"] : "compact"},
        {"write_list", config.count("write_list") ? config["write_list"] : "compact"},
        {"conversion_lists", config.count("conversion_lists") ? config["conversion_lists"] : "compact"}
    };

    // Attempt to open the config file for writing
    std::ofstream outFile(configPath);
    if (outFile.is_open()) {
        // Write updated config values to the file
        for (const auto& [key, value] : orderedDefaults) {
            outFile << key << " = " << value << "\n";
        }
        outFile.close();
    } else {
        // If file couldn't be opened, display error and return
        std::cerr << "\n\033[1;91mError: Unable to access configuration file: \033[1;93m'"
                  << configPath << "'\033[1;91m.\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }

    // If file was successfully updated, set the pagination value
    ITEMS_PER_PAGE = paginationValue;
    if (paginationValue > 0) {
        std::cout << "\n\033[0;1mPagination status updated: Max entries per page set to \033[1;93m" 
                  << paginationValue << "\033[1;97m.\033[0m" << std::endl;
    } else {
        std::cout << "\n\033[0;1mPagination status updated: \033[1;91mDisabled\033[0;1m." << std::endl;
    }
    
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
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


// Custom readline matching list function
void customListingsFunction(char **matches, int num_matches, int max_length) {
    (void)max_length; // Silencing unused parameter warning
    
    // Save the current cursor position
    printf("\033[s");
    // Clear any listings if visible and leave a new line
    std::cout << "\033[J";
    printf("\n");
    
    // Calculate how many items to display based on ITEMS_PER_PAGE
    // If ITEMS_PER_PAGE <= 0, show all matches
    int items_to_display;
    if (ITEMS_PER_PAGE <= 0) {
        items_to_display = num_matches;
    } else {
        // Fix signedness comparison issue by casting
        items_to_display = ((size_t)num_matches > ITEMS_PER_PAGE) ? 
                           (int)ITEMS_PER_PAGE : num_matches;
    }
    
    // Print header if we have multiple matches
    if (num_matches > 1) {
        printf("\n\033[1;38;5;130mTab Completion Matches (\033[1;93mCtrl+l\033[0;1m:clear\033[1;38;5;130m):\033[0m\n\n");
    }
    
    // Find common prefix among matches
    const char* base_path = matches[1];
    int base_len = 0;
    
    // Find the last occurrence of '/' before the part we're tab-completing
    const char* last_slash = strrchr(base_path, '/');
    if (last_slash != NULL) {
        base_len = last_slash - base_path + 1; // Include the slash
    }
    
    // Determine the maximum length of all items
    size_t max_item_length = 0;
    
    for (int i = 1; i <= items_to_display; i++) {
        const char* relative_path = matches[i] + base_len;
        size_t item_length = strlen(relative_path);
        
        if (item_length > max_item_length) {
            max_item_length = item_length;
        }
    }
    
    // Calculate number of columns based on items to display
    int num_columns = 3; // Default to 3 columns
    if (items_to_display <= 2) {
        // If we have 1 or 2 items, use fewer columns
        num_columns = items_to_display;
    }
    
    // Define column parameters
    const int column_spacing = 4;
    int column_width = 40; // Default width
    
    // Adjust column width based on number of columns and item length
    if (num_columns < 3) {
        // For fewer columns, we can use wider columns if needed
        // Use the max item length plus padding, but cap at a reasonable size
        column_width = (max_item_length + 2 > 60) ? 60 : max_item_length + 2;
    } else {
        // For 3 columns, use adaptive width but with more conservative limits
        if (max_item_length < 38) {
            column_width = max_item_length + 2; // Add 2 for padding
        }
    }
    
    const int total_column_width = column_width + column_spacing;
    
    // Calculate rows needed
    int rows = (items_to_display + num_columns - 1) / num_columns;
    
    // Function to check if a path is a directory
    auto isDirectory = [](const char* path) -> bool {
        struct stat path_stat;
        if (stat(path, &path_stat) != 0) {
            return false; // If stat fails, assume it's not a directory
        }
        return S_ISDIR(path_stat.st_mode);
    };
    
    // Function for smart truncation
    auto smartTruncate = [](const char* str, int max_width) -> std::string {
        std::string result(str);
        size_t len = result.length();
        
        if (len <= (size_t)max_width) {
            return result; // No truncation needed
        }
        
        // Find file extension if present
        size_t dot_pos = result.find_last_of('.');
        bool has_extension = (dot_pos != std::string::npos && dot_pos > 0 && len - dot_pos <= 10);
        
        // If we have a reasonable extension length (<=10 chars), preserve it
        if (has_extension && dot_pos > 0) {
            std::string ext = result.substr(dot_pos);
            
            // Minimum chars to keep at beginning (at least 3)
            int prefix_len = std::max(3, max_width - (int)ext.length() - 3);
            
            if (prefix_len >= 3) {
                // We have enough space for prefix + ... + extension
                return result.substr(0, prefix_len) + "..." + ext;
            }
        }
        
        // For other cases or very long extensions, use middle truncation
        int prefix_len = (max_width - 3) / 2;
        int suffix_len = max_width - 3 - prefix_len;
        
        return result.substr(0, prefix_len) + "..." + 
               result.substr(len - suffix_len, suffix_len);
    };
    
    // Print matches in the determined number of columns
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < num_columns; col++) {
            int index = row + col * rows + 1;
            
            if (index <= items_to_display) {
                const char* full_path = matches[index];
                const char* relative_path = full_path + base_len;
                
                // Check if the path is a directory
                bool is_dir = isDirectory(full_path);
                
                // Apply color and format based on file type
                std::string formatted;
                
                if (is_dir) {
                    // Use blue color for directories and append a slash
                    formatted = "\033[1;34m" + smartTruncate(relative_path, column_width - 1) + "/\033[0m";
                } else {
                    // Regular color for files
                    formatted = smartTruncate(relative_path, column_width);
                }
                
                // Last column doesn't need padding
                if (col == num_columns - 1 || index == items_to_display) {
                    printf("%s", formatted.c_str());
                } else {
                    // Need to handle ANSI escape codes when padding
                    // Standard padding won't work correctly with color codes, so we add spaces manually
                    int visible_length = strlen(relative_path);
                    if (is_dir) visible_length++; // Account for the slash
                    
                    printf("%s", formatted.c_str());
                    
                    // Add appropriate spacing (accounting for truncation)
                    int displayed_length = std::min((int)visible_length, column_width);
                    int padding = total_column_width - displayed_length;
                    for (int i = 0; i < padding; i++) {
                        printf(" ");
                    }
                }
            }
        }
        printf("\n");
    }
    
    // Only show the pagination message if we're actually limiting results
    // and ITEMS_PER_PAGE is positive
    if (ITEMS_PER_PAGE > 0 && (size_t)num_matches > ITEMS_PER_PAGE) {
        printf("\n\033[1;33m[Showing %d/%d matches... increase pagination limit to display more]\033[0;1m\n", 
               items_to_display, num_matches);
    }
    
    // Move the cursor back to the saved position
    printf("\033[u");
}


// Function to restore readline history keys but prevent declutter and listings
void restoreReadline() {
    rl_completion_display_matches_hook = customListingsFunction;
    rl_attempted_completion_function = nullptr;
    rl_bind_keyseq("\033[A", rl_get_previous_history);
    rl_bind_keyseq("\033[B", rl_get_next_history);
    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);
}


// Function to disable readline history,declutter and listings
void disableReadlineForConfirmation() {
    rl_bind_key('\f', prevent_readline_keybindings);
    rl_bind_key('\t', prevent_readline_keybindings);
    rl_bind_keyseq("\033[A", prevent_readline_keybindings);
    rl_bind_keyseq("\033[B", prevent_readline_keybindings);
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


// Compare two strings in natural order, case-insensitively
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


// Sort files using a natural order, case-insensitive comparator
void sortFilesCaseInsensitive(std::vector<std::string>& files) {
    if (files.empty())
        return;

    const size_t n = files.size();
    // Use at most one thread per element if n < maxThreads
    unsigned int numThreads = std::min<unsigned int>(maxThreads, static_cast<unsigned int>(n));
    // Determine the chunk size so that each thread roughly gets the same number of elements
    size_t chunkSize = (n + numThreads - 1) / numThreads;

    // Each pair holds the start and end indices of a sorted chunk.
    std::vector<std::pair<size_t, size_t>> chunks;
    // Futures to wait for each sorting task.
    std::vector<std::future<void>> futures;

    // Launch parallel sorting tasks.
    for (unsigned int i = 0; i < numThreads; ++i) {
        size_t start = i * chunkSize;
        size_t end = std::min(n, (i + 1) * chunkSize);
        if (start >= end)
            break;
        // Record this chunk's range.
        chunks.emplace_back(start, end);
        futures.push_back(std::async(std::launch::async, [start, end, &files]() {
            std::sort(files.begin() + start, files.begin() + end, [](const std::string& a, const std::string& b) {
                return naturalCompare(a, b) < 0;
            });
        }));
    }

    // Wait for all sorting tasks to complete
    for (auto &f : futures)
        f.get();

    // Merge sorted chunks pairwise until the entire vector is merged
    while (chunks.size() > 1) {
        std::vector<std::pair<size_t, size_t>> newChunks;
        for (size_t i = 0; i < chunks.size(); i += 2) {
            if (i + 1 < chunks.size()) {
                size_t start = chunks[i].first;
                size_t mid = chunks[i + 1].first;
                size_t end = chunks[i + 1].second;
                std::inplace_merge(files.begin() + start, files.begin() + mid, files.begin() + end,
                    [](const std::string& a, const std::string& b) {
                        return naturalCompare(a, b) < 0;
                    });
                newChunks.emplace_back(start, end);
            } else {
                // Odd number of chunks: last one remains as is
                newChunks.push_back(chunks[i]);
            }
        }
        chunks = std::move(newChunks);
    }
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
