#include "../headers.h"

 
// Get max available CPU cores for global use, fallback is 2 cores
unsigned int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;

std::mutex Mutex4High; // Mutex for high level functions
std::mutex Mutex4Med; // Mutex for mid level functions
std::mutex Mutex4Low; // Mutex for low level functions

// For cache directory creation
bool gapPrinted = false; // for cache refresh for directory function
bool promptFlag = true; // for cache refresh for directory function
bool gapPrintedtraverse = false; // for traverse function

// For saving history to a differrent cache for FilterPatterns
bool historyPattern = false;

// Default readline history save path
const std::string historyFilePath = std::string(getenv("HOME")) + "/.cache/iso_commander_history_cache.txt";
const std::string historyPatternFilePath = std::string(getenv("HOME")) + "/.cache/iso_commander_pattern_cache.txt";

//Maximum number of history entries at a time
const int MAX_HISTORY_LINES = 100;

const int MAX_HISTORY_PATTERN_LINES = 20;

// Global variables for cleanup
int lockFileDescriptor = -1;


// Vector to store ISO unique input errors
std::set<std::string> uniqueErrorMessages;



// Main function
int main(int argc, char *argv[]) {
	
	if (argc == 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-v")) {
        printVersionNumber("3.3.7");
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
        clearScrollBuffer();
        print_ascii();
        // Display the main menu options
        printMenu();

        // Clear history
        clear_history();

        // Prompt for the main menu choice
        char* input = readline("\n\001\033[1;94m\002Choose an option:\001\033[0;1m\002 ");
        if (!input) {
            break; // Exit the program if readline returns NULL (e.g., on EOF or Ctrl+D)
        }

        std::string choice(input);
        free(input); // Free the allocated memory by readline

        if (choice == "1") {
            submenu1();
        } else {
            // Check if the input length is exactly 1
            if (choice.length() == 1) {
                switch (choice[0]) {
                    case '2':
                        submenu2();
                        break;
                    case '3':
                        manualRefreshCache();
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
	                                                                                                                           
std::cout << Color << R"( (   (       )            )    *      *              ) (         (    
 )\ ))\ ) ( /(      (  ( /(  (  `   (  `    (     ( /( )\ )      )\ ) 
(()/(()/( )\())     )\ )\()) )\))(  )\))(   )\    )\()(()/(  (  (()/( 
 /(_)/(_)((_)\    (((_((_)\ ((_)()\((_)()((((_)( ((_)\ /(_)) )\  /(_))
(_))(_))   ((_)   )\___ ((_)(_()((_(_()((_)\ _ )\ _((_(_))_ ((_)(_))
|_ _/ __| / _ \  ((/ __/ _ \|  \/  |  \/  (_)_\(_| \| ||   \| __| _ \
 | |\__ \| (_) |  | (_| (_) | |\/| | |\/| |/ _ \ | .` || |) | _||   /
|___|___/ \___/    \___\___/|_|  |_|_|  |_/_/ \_\|_|\_||___/|___|_|_\

)" << resetColor;

}


// Function to print submenu1
void submenu1() {

    while (true) {
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
        std::cout << " ";
        char* submenu_input = readline("\n\001\033[1;94m\002Choose an option:\001\033[0;1m\002 ");

        if (!submenu_input || std::strlen(submenu_input) == 0) {
			free(submenu_input);
			break; // Exit the submenu if input is empty or NULL
		}
					
          std::string submenu_choice(submenu_input);
          free(submenu_input);
         // Check if the input length is exactly 1
        if (submenu_choice.empty() || submenu_choice.length() == 1) {
		std::string operation;
		switch (submenu_choice[0]) {
        case '1':
			clearScrollBuffer();
            select_and_mount_files_by_number();
            clearScrollBuffer();
            break;
        case '2':
			clearScrollBuffer();
            unmountISOs();
            clearScrollBuffer();
            break;
        case '3':
			clearScrollBuffer();
            operation = "rm";
            select_and_operate_files_by_number(operation);
            clearScrollBuffer();
            break;
        case '4':
			clearScrollBuffer();
            operation = "mv";
            select_and_operate_files_by_number(operation);
            clearScrollBuffer();
            break;
        case '5':
			clearScrollBuffer();
            operation = "cp";
            select_and_operate_files_by_number(operation);
            clearScrollBuffer();
            break;
			}
		}
    }
}


// Function to print submenu2
void submenu2() {
	while (true) {
		clearScrollBuffer();
		std::cout << "\033[1;32m+-------------------------+\n";
		std::cout << "\033[1;32m|↵ Convert2ISO             |\n";
		std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|1. CCD2ISO               |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|2. MDF2ISO               |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << " ";
        char* submenu_input = readline("\n\001\033[1;94m\002Choose an option:\001\033[0;1m\002 ");
        

        if (!submenu_input || std::strlen(submenu_input) == 0) {
			free(submenu_input);
			break; // Exit the submenu if input is empty or NULL
		}
					
          std::string submenu_choice(submenu_input);
          free(submenu_input);
         // Check if the input length is exactly 1
		 if (submenu_choice.empty() || submenu_choice.length() == 1){
         switch (submenu_choice[0]) {		
             case '1':
				clearScrollBuffer();
                select_and_convert_files_to_iso("bin");
                clearScrollBuffer();
                break;
             case '2':
				clearScrollBuffer();
                select_and_convert_files_to_iso("mdf");
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
    std::cout << "\033[1;32m|1. Manage ISO            |\n";
    std::cout << "\033[1;32m+-------------------------+\n";
    std::cout << "\033[1;32m|2. Convert2ISO           |\n";
    std::cout << "\033[1;32m+-------------------------+\n";
    std::cout << "\033[1;32m|3. Refresh ISO Cache     |\n";
    std::cout << "\033[1;32m+-------------------------+\n";
    std::cout << "\033[1;32m|4. Exit Program          |\n";
    std::cout << "\033[1;32m+-------------------------+\n";
    std::cout << " ";
}


// GENERAL STUFF

// Function to clear scrollbuffer
void clearScrollBuffer() {
    std::cout << "\033[3J";  // Clear the scrollback buffer
    std::cout << "\033[2J";  // Clear the screen
    std::cout << "\033[H";   // Move the cursor to the top-left corner
    std::cout.flush();       // Ensure the output is flushed
}


// Function to handle termination signals
void signalHandler(int signum) {
    
    // Perform cleanup before exiting
    if (lockFileDescriptor != -1) {
        close(lockFileDescriptor);
    }
    
    exit(signum);
}


// Function to check if a string consists only of zeros
bool isAllZeros(const std::string& str) {
    return str.find_first_not_of('0') == std::string::npos;
}


// Function to check if a string is numeric
bool isNumeric(const std::string& str) {
    return std::all_of(str.begin(), str.end(), [](char c) {
        return std::isdigit(c);
    });
}


// Function to print ISO files with alternating colors for sequence numbers
void printIsoFileList(const std::vector<std::string>& isoFiles) {
    // ANSI escape codes for text formatting
    const std::string defaultColor = "\033[0m";
    const std::string bold = "\033[1m";
    const std::string red = "\033[31;1m";   // Red color
    const std::string green = "\033[32;1m"; // Green color
    const std::string magenta = "\033[95m";

    bool useRedColor = true; // Start with red color

    for (size_t i = 0; i < isoFiles.size(); ++i) {
        // Determine sequence number
        int sequenceNumber = i + 1;
        int stew = 1;
        
        if (isoFiles.size() >= 10) {
			stew = 2;
		} else if (isoFiles.size() >= 100) {
			stew = 3;
		} else if (isoFiles.size() >= 1000) {
			stew = 4;
		} else if (isoFiles.size() >= 10000) {
			stew = 5;
		} else if (isoFiles.size() >= 100000) {
			stew = 6;
		}	

        // Determine color based on alternating pattern
        std::string sequenceColor = (useRedColor) ? red : green;
        useRedColor = !useRedColor; // Toggle between red and green

        // Extract directory and filename
        auto [directory, filename] = extractDirectoryAndFilename(isoFiles[i]);

        // Print the directory part in the default color
        std::cout << "\n" << sequenceColor << std::right << std::setw(stew) << sequenceNumber << ". " << defaultColor << bold << directory << defaultColor << bold << "/" << magenta << filename << defaultColor;

    }
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
std::pair<std::string, std::string> extractDirectoryAndFilename(const std::string& path) {
    // Initialize variables to store directory and filename
    std::string directory;
    std::string filename;

    // Initialize variables to track positions of slashes in the path
    std::size_t lastSlashPos = 0;
    std::size_t currentSlashPos = path.find_first_of("/\\");

    // Use stringstream for efficient string concatenation
    std::stringstream dirStream;

    // Loop through the path to extract directory components
    while (currentSlashPos != std::string::npos) {
        // Extract the current component between slashes
        std::string component = path.substr(lastSlashPos, currentSlashPos - lastSlashPos);

        // Limit each component to 28 characters or the first space gap or the first hyphen
        std::size_t maxComponentSize = 28;
        std::size_t spacePos = component.find(' ');
        std::size_t hyphenPos = component.find('-');

        if (spacePos != std::string::npos && spacePos <= maxComponentSize) {
            component = component.substr(0, spacePos);
        } 
        if (hyphenPos != std::string::npos && hyphenPos <= maxComponentSize) {
            component = component.substr(0, hyphenPos);
        } else {
            component = component.substr(0, maxComponentSize);
        }

        // Append the processed component to the stringstream
        dirStream << component << '/';
        // Update positions for the next iteration
        lastSlashPos = currentSlashPos + 1;
        currentSlashPos = path.find_first_of("/\\", lastSlashPos);
    }

    // Extract the last component as the filename
    filename = path.substr(lastSlashPos);

    // Remove the last '/' if the directory is not empty
    directory = dirStream.str();
    if (!directory.empty() && directory.back() == '/') {
        directory.pop_back();
    }

    // Replace specific Linux standard directories with custom strings
    std::unordered_map<std::string, std::string> replacements = {
        {"/home", "~"},
        {"/root", "/R"},
        // Add more replacements as needed
    };

    // Iterate through the replacements and update the directory
    for (const auto& [oldDir, newDir] : replacements) {
        size_t pos = directory.find(oldDir);
        if (pos != std::string::npos) {
            directory.replace(pos, oldDir.length(), newDir);
        }
    }

    // Return the pair of directory and filename
    return {directory, filename};
}


// Function to load history from readline
void loadHistory() {
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
void saveHistory() {
    std::ofstream historyFile;
    if (!historyPattern) {
        historyFile.open(historyFilePath, std::ios::out | std::ios::trunc);
    } else {
        historyFile.open(historyPatternFilePath, std::ios::out | std::ios::trunc);
    }


    if (historyFile.is_open()) {
        HIST_ENTRY **histList = history_list();

        if (histList) {
            std::unordered_map<std::string, int> lineIndices; // To store the index of each line
            std::vector<std::string> uniqueLines;

            // Iterate through all history entries
            for (int i = 0; histList[i]; i++) {
                std::string line(histList[i]->line);

                if (!line.empty()) {
                    auto it = lineIndices.find(line);
                    if (it == lineIndices.end()) {
                        // Line not found, insert it
                        lineIndices[line] = uniqueLines.size();
                        uniqueLines.push_back(line);
                    } else {
                        // Line found, remove the old instance and add the new one
                        uniqueLines.erase(uniqueLines.begin() + it->second);
                        lineIndices[line] = uniqueLines.size();
                        uniqueLines.push_back(line);
                    }
                }
            }
			
			int excessLines; // Declare excessLines outside the if-else block

			if (!historyPattern) {
				// Adjust the number of lines to keep within the limit
				excessLines = uniqueLines.size() - MAX_HISTORY_LINES;
			} else {
				excessLines = uniqueLines.size() - MAX_HISTORY_PATTERN_LINES;
			}

            if (excessLines > 0) {
                // Remove excess lines from the beginning
                uniqueLines.erase(uniqueLines.begin(), uniqueLines.begin() + excessLines);
            }

            // Write all the lines to the file
            for (const auto& line : uniqueLines) {
                historyFile << line << std::endl;
            }
        }

        historyFile.close();
    } else {
        std::cerr << "\n\033[1;91mFailed to open history cache file: \033[1;93m'" << historyFilePath << "'\033[1;91m. Check read/write permissions.\033[0m";
    }
}


// Function for tab completion and history creation
std::string readInputLine(const std::string& prompt) {
    try {
        // Ensure the prompt is displayed before reading input
        std::cout << prompt;
        std::cout.flush();

        // Read a line of input using the readline function
        std::unique_ptr<char, decltype(&free)> input(readline(""), &free);

        // Check if input is not null (readline successful)
        if (input && input.get()[0] != '\0' && std::string(input.get()) != "\n") {
            // Add the input to the history
            add_history(input.get());

            // Convert the C-style string to a C++ string
            return std::string(input.get());
        }
    } catch (const std::exception& e) {
        // Log the error or handle it in a way suitable for your application
        std::cerr << "\033[91m" << e.what() << "\033[0m";
    }

    // Return an empty string if readline fails or input is empty
    return "";
}
