// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"

// Get max available CPU cores for global use, fallback is 2 cores
unsigned int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;

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
        printVersionNumber("5.5.4");
        return 0;
    }
    // Readline use semicolon as delimiter
    rl_completer_word_break_characters = (char *)";";

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
    
    // Open the file
    std::ifstream file(historyFilePath);
    if (!file.is_open()) {
        std::cerr << "Failed to open file: " << historyFilePath << std::endl;
        return 1;
    }
    
    const std::string automaticFilePath = std::string(getenv("HOME")) + "/.cache/iso_commander_automatic.txt";
    bool search = readUserChoice(automaticFilePath);
	if (search) {
    // String to store all paths
    std::string allPaths;

    // Read the file line by line
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string path;
        while (std::getline(iss, path, ';')) {
            if (!path.empty()) {
                if (!allPaths.empty()) {
                    allPaths += ";";
                }
                allPaths += path;
            }
        }
    }

    // Close the file
    file.close();
    
		manualRefreshCache(allPaths, false, maxDepth, historyPattern);
	}

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
            submenu1(maxDepth, historyPattern, verbose);
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
						clearScrollBuffer();
                        saveUserChoice(automaticFilePath);
                        clearScrollBuffer();
                        break;
                    case '5':
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
void submenu1(int& maxDepth, bool& historyPattern, bool& verbose) {
	
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
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\n";
        char* rawInput = readline("\001\033[1;94m\002Choose an option:\001\033[0;1m\002 ");

        // Use std::unique_ptr to manage memory for input
		std::unique_ptr<char[], decltype(&std::free)> input(rawInput, &std::free);

        std::string mainInputString(input.get());

        if (!input.get() || std::strlen(input.get()) == 0) {
			break; // Exit the submenu if input is empty or NULL
		}

          std::string submenu_choice(mainInputString);
         // Check if the input length is exactly 1
        if (submenu_choice.empty() || submenu_choice.length() == 1) {
		switch (submenu_choice[0]) {
        case '1':
			clearScrollBuffer();
            selectForIsoFiles("mount", historyPattern, maxDepth, verbose);
            clearScrollBuffer();
            break;
        case '2':
			clearScrollBuffer();
            selectForIsoFiles("umount", historyPattern, maxDepth, verbose);
            clearScrollBuffer();
            break;
        case '3':
			clearScrollBuffer();
            selectForIsoFiles("rm", historyPattern, maxDepth, verbose);
            clearScrollBuffer();
            break;
        case '4':
			clearScrollBuffer();
            selectForIsoFiles("mv", historyPattern, maxDepth, verbose);

            clearScrollBuffer();
            break;
        case '5':
			clearScrollBuffer();
            selectForIsoFiles("cp", historyPattern, maxDepth, verbose);
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
        char* rawInput = readline("\001\033[1;94m\002Choose an option:\001\033[0;1m\002 ");

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
    std::cout << "\033[1;32m|4. AutoImportISO         |\n";
    std::cout << "\033[1;32m+-------------------------+\n";
    std::cout << "\033[1;32m|5. Exit                  |\n";
    std::cout << "\033[1;32m+-------------------------+";
    std::cout << "\n";
}


// GENERAL STUFF

bool readUserChoice(const std::string& filePath) {
    std::ifstream inFile(filePath);
    if (!inFile) {
        return false; // Default to false if file cannot be opened
    }

    int userChoice;
    inFile >> userChoice;

    // Check if the read operation succeeded and the value is either 0 or 1
    if (inFile.fail() || (userChoice != 0 && userChoice != 1)) {
        return false; // Default to false if the content is invalid
    }

    return (userChoice == 1); // Return true if userChoice is 1, otherwise false
}

void saveUserChoice(const std::string& filePath) {
	// Calls prevent_clear_screen and tab completion
	rl_bind_key('\f', prevent_clear_screen_and_tab_completion);
	rl_bind_key('\t', prevent_clear_screen_and_tab_completion);
	while (true) {
		clearScrollBuffer();
    std::string prompt = "\001\033[1;94m\002Scans isocmd's folder history (up to 25 entries) for new ISO files and imports them into \001\033[1;92m\002on-disk \001\033[1;94m\002cache at every startup.\n"
						"\001\033[1;93m\002Note: This feature may be slow for older drives and is disabled by default.\001\033[0;1m\002"
						"\n\n\001\033[1;94m\002Configure automatic ISO cache updates on startup, (\001\033[1;92m\0021\001\033[1;94m\002/\001\033[1;91m\0020\001\033[1;94m\002), or anyKey ↵ to return: \001\033[0;1m\002";
    std::unique_ptr<char, decltype(&std::free)> input(readline(prompt.c_str()), &std::free);
    std::string mainInputString(input.get());
    
    if (!input.get() || std::strlen(input.get()) == 0 || (mainInputString != "1" && mainInputString != "0")) {
			break; // Exit the submenu if input is empty or NULL
	}

    // Convert input to lowercase and check if it's 'y'
    int valueToSave = (!mainInputString.empty() && (mainInputString[0] == '1' || mainInputString[0] == '1')) ? 1 : 0;

    std::ofstream outFile(filePath);
    if (outFile.is_open()) {
        outFile << valueToSave;
        outFile.close();
        if (valueToSave == 1) {
			std::cout << "\n\033[0;1mAutomatic ISO cache updates have been \033[1;92menabled\033[0;1m.\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            continue;
		} else {
			std::cout << "\n\033[0;1mAutomatic ISO cache updates have been \033[1;91mdisabled\033[0;1m.\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            continue;
		}
    } else {
        std::cerr << "\n\033[1;91mFailed to set configuration for automatic ISO cache updates, unable to access: \033[1;91m'\033[1;93m" << filePath << "\033[1;91m'.\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        continue;
    }
	}
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


// Sorts items in a case-insensitive manner
void sortFilesCaseInsensitive(std::vector<std::string>& files) {
    std::sort(files.begin(), files.end(), 
        [](const std::string& a, const std::string& b) {
            return strcasecmp(a.c_str(), b.c_str()) < 0;
        }
    );
}


// Function to get the sudo invoker ID
void getRealUserId(uid_t& real_uid, gid_t& real_gid, std::string& real_username, std::string& real_groupname,std::set<std::string>& uniqueErrors) {

    // Get the real user ID and group ID (of the user who invoked sudo)
    const char* sudo_uid = std::getenv("SUDO_UID");
    const char* sudo_gid = std::getenv("SUDO_GID");

    if (sudo_uid && sudo_gid) {
        try {
            real_uid = static_cast<uid_t>(std::stoul(sudo_uid));
            real_gid = static_cast<gid_t>(std::stoul(sudo_gid));
        } catch (const std::exception& e) {
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
        uniqueErrors.insert("\033[1;91mError getting user information: " + std::string(strerror(errno)) + "\033[0;1m");
        return;
    }
    real_username = pw->pw_name;

    // Get real group name
    struct group *gr = getgrgid(real_gid);
    if (gr == nullptr) {
        uniqueErrors.insert("\033[1;91mError getting group information: " + std::string(strerror(errno)) + "\033[0;1m");
        return;
    }
    real_groupname = gr->gr_name;
}
