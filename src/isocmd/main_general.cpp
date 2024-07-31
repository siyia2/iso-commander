// SPDX-License-Identifier: GNU General Public License v3.0 or later

#include "../headers.h"


// Get max available CPU cores for global use, fallback is 2 cores
unsigned int maxThreads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2;

// Mutex for HighLevel functions
std::mutex Mutex4High;

// Mutex for LowLevel functions
std::mutex Mutex4Low;

// For cache directory creation
bool gapPrinted = false;
bool promptFlag = true;
bool verbose = false;

// For saving history to a differrent cache for FilterPatterns
bool historyPattern = false;

// Default readline history save path
const std::string historyFilePath = std::string(getenv("HOME")) + "/.cache/iso_commander_history_cache.txt";
const std::string historyPatternFilePath = std::string(getenv("HOME")) + "/.cache/iso_commander_pattern_cache.txt";

//Maximum number of history entries at a time
const int MAX_HISTORY_LINES = 25;

const int MAX_HISTORY_PATTERN_LINES = 25;

// Global variables for cleanup
int lockFileDescriptor = -1;


// Main function
int main(int argc, char *argv[]) {

	if (argc == 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-v")) {
        printVersionNumber("5.1.6");
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
		globalIsoFileList.reserve(100);
        clearScrollBuffer();
        print_ascii();
        // Display the main menu options
        printMenu();

        // Clear history
        clear_history();

        rl_bind_key('\t', custom_complete);

        // Prompt for the main menu choice
        char* rawInput = readline("\n\001\033[1;94m\002Choose an option:\001\033[0;1m\002 ");

        std::unique_ptr<char[], decltype(&std::free)> input(rawInput, &std::free);

        std::string mainInputString(input.get());

        if (!input.get()) {
            break; // Exit the program if readline returns NULL (e.g., on EOF or Ctrl+D)
        }


        std::string choice(mainInputString);

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
        std::cout << "\033[1;32m|1. CCD2ISO++             |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << "\033[1;32m|2. MDF2ISO++             |\n";
        std::cout << "\033[1;32m+-------------------------+\n";
        std::cout << " ";
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
					select_and_convert_files_to_iso(operation);
                clearScrollBuffer();
                break;
             case '2':
				clearScrollBuffer();
				operation = "mdf";
					select_and_convert_files_to_iso(operation);
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
    std::cout << "\033[1;32m+-------------------------+\n";
    std::cout << " ";
}


// GENERAL STUFF

// Function to clear scrollbuffer
void clearScrollBuffer() {
        std::cout << "\033[3J\033[2J\033[H\033[0m" << std::flush;
}


// Custom function to handle tab presses
int custom_complete(int count, int key) {
    // If it's not a tab key, use default behavior
    if (key != '\t') {
        return rl_insert(count, key);
    }

    // If it's the first tab, do normal completion
    if (rl_last_func != rl_complete) {
        return rl_complete(count, key);
    }

    // If it's a second tab, do nothing
    return 0;
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


// Function to display progress bar for native operations
void displayProgressBar(const std::atomic<size_t>& completedIsos, const size_t& totalIsos, std::atomic<bool>& isComplete) {
    const int barWidth = 50;
    bool enterPressed = false;
    auto startTime = std::chrono::high_resolution_clock::now();

    while (!isComplete.load() || !enterPressed) {
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
                  << "Time Elapsed: "<< std::setprecision(1) << elapsedSeconds << "s";

        if (completedValue == totalIsos && !enterPressed) {
            enterPressed = true;
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

    // Print a newline after completion
    std::cout << std::endl;
}


// Function to print ISO files with alternating colors for sequence numbers
void printIsoFileList(const std::vector<std::string>& isoFiles) {
    static const char* defaultColor = "\033[0m";
    static const char* bold = "\033[1m";
    static const char* red = "\033[31;1m";
    static const char* green = "\033[32;1m";
    static const char* magenta = "\033[95m";

    size_t maxIndex = isoFiles.size();
    size_t numDigits = std::to_string(maxIndex).length();

    std::string output;
    output.reserve(isoFiles.size() * 100);  // Adjust based on average line length

    for (size_t i = 0; i < isoFiles.size(); ++i) {
        const char* sequenceColor = (i % 2 == 0) ? red : green;

        output.append("\n")
              .append(sequenceColor);

        // Create a temporary stringstream for setw formatting
        std::ostringstream temp;
        temp << std::right << std::setw(numDigits) << (i + 1);
        output.append(temp.str());

        output.append(". ")
              .append(defaultColor)
              .append(bold);

        auto [directory, filename] = extractDirectoryAndFilename(isoFiles[i]);

        output.append(directory)
              .append(defaultColor)
              .append(bold)
              .append("/")
              .append(magenta)
              .append(filename)
              .append(defaultColor);
    }

    std::cout << output;
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
    static const std::unordered_map<std::string_view, std::string_view> replacements = {
        {"/home", "~"},
        {"/root", "/R"},
        // Add more replacements as needed
    };

    size_t lastSlashPos = path.find_last_of("/\\");
    if (lastSlashPos == std::string::npos) {
        return {"", path};
    }

    std::string processedDir;
    processedDir.reserve(path.length());  // Pre-allocate memory

    size_t start = 0;
    while (start < lastSlashPos) {
        size_t end = path.find_first_of("/\\", start);
        if (end == std::string::npos || end > lastSlashPos) {
            end = lastSlashPos;
        }

        size_t componentLength = end - start;
        size_t truncatePos = std::min({
            componentLength,
            path.find(' ', start) - start,
            path.find('-', start) - start,
            size_t(28)
        });

        processedDir.append(path, start, truncatePos);
        processedDir.push_back('/');

        start = end + 1;
    }

    if (!processedDir.empty()) {
        processedDir.pop_back();  // Remove trailing slash
    }

    // Apply replacements
    for (const auto& [oldDir, newDir] : replacements) {
        size_t pos = 0;
        while ((pos = processedDir.find(oldDir, pos)) != std::string::npos) {
            processedDir.replace(pos, oldDir.length(), newDir);
            pos += newDir.length();
        }
    }

    return {processedDir, path.substr(lastSlashPos + 1)};
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
