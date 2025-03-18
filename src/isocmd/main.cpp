// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"
#include "../display.h"


// Get max available CPU cores for global use, fallback is 2 cores
unsigned int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;

// Global mutex to protect the verbose sets
std::mutex globalSetsMutex;

const std::string configPath = std::string(getenv("HOME")) + "/.config/isocmd/config";

// Global falg to track cancellation
std::atomic<bool> g_operationCancelled{false};

// Hold current page for pagination
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
        printVersionNumber("5.9.2");
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


// Print the version number of the program
void printVersionNumber(const std::string& version) {
	
    std::cout << "\x1B[32mIso Commander v" << version << "\x1B[0m\n"; // Output the version number in green color
}

