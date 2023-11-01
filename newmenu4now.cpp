#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <mutex>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <readline/readline.h>
#include <readline/history.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <future>
#include <unistd.h>
#include <dirent.h>
#include <sstream>
#include <thread>
#include <mutex>

std::mutex mtx;

// Define the default cache directory
const std::string cacheDirectory = "/tmp/";

namespace fs = std::filesystem;


// Function prototypes
void listAndMountISOs();
void unmountISOs();
void cleanAndUnmountAllISOs();
void convertBINsToISOs();
void listMountedISOs();
void listMode();
void manualMode_isos();
void select_and_mount_files_by_number();
void select_and_convert_files_to_iso();
void manualMode_imgs();
void print_ascii();
void screen_clear();

std::vector<std::string> findBinImgFiles(const std::string& directory) {
    std::vector<std::string> fileNames;

    try {
        for (const auto& entry : fs::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower); // Convert extension to lowercase
                if (ext == ".bin" || ext == ".img") {
                    if (fs::file_size(entry) >= 50'000'000) {
                        fileNames.push_back(entry.path().string());
                    }
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Filesystem error: " << e.what() << std::endl;
    }

    return fileNames;
}


// Function to list and prompt the user to choose a file for conversion
std::string chooseFileToConvert(const std::vector<std::string>& files) {
    std::cout << "Found the following .bin and .img files:\n";
    for (size_t i = 0; i < files.size(); ++i) {
        std::cout << i + 1 << ": " << files[i] << "\n";
    }

    int choice;
    std::cout << "Enter the number of the file you want to convert: ";
    std::cin >> choice;

    if (choice >= 1 && choice <= static_cast<int>(files.size())) {
        return files[choice - 1];
    } else {
        std::cout << "Invalid choice. Please choose a valid file.\n";
        return "";
    }
}

int main() {
    std::string choice;
    std::string directoryPath;
    std::vector<std::string> binImgFiles;  // Declare binImgFiles here

    while (true) {
        // Display the menu options
        std::cout << "Menu Options:" << std::endl;
        std::cout << "1. List and Mount ISOs" << std::endl;
        std::cout << "2. Unmount ISOs" << std::endl;
        std::cout << "3. Clean and Unmount All ISOs" << std::endl;
        std::cout << "4. Scan for .bin and .img Files" << std::endl;
        std::cout << "5. List Mounted ISOs" << std::endl;
        std::cout << "6. Exit the Program" << std::endl;

        // Prompt for choice
        std::cout << "Enter your choice: ";
        std::cin >> choice;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');  // Consume the newline character

        if (choice == "1") {
            // Call your listAndMountISOs function
        } else if (choice == "2") {
            // Call your unmountISOs function
            unmountISOs();
            std::cout << "Press Enter to continue...";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::system("clear");
        } else if (choice == "3") {
            // Call your cleanAndUnmountAllISOs function
            cleanAndUnmountAllISOs();
            std::cout << "Press Enter to continue...";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::system("clear");
            } else if (choice == "4") {			
            std::cout << "Enter the directory path to scan for .bin and .img files: ";
            std::getline(std::cin, directoryPath);
            binImgFiles = findBinImgFiles(directoryPath);

            if (binImgFiles.empty()) {
                std::cout << "No .bin or .img files found in the specified directory and its subdirectories or all files are under 50MB." << std::endl;
            } else {
                for (int i = 0; i < binImgFiles.size(); i++) {
                    std::cout << i + 1 << ". " << binImgFiles[i] << std::endl;
                }
				
                std::vector<std::string> selectedFiles;
                while (true) {
					std::string input;
					std::cout << "Choose a file to process (enter the number or range e.g., 1-5 or press Enter to exit): ";
					std::getline(std::cin, input);
                    
                    if (input.empty()) {
                        std::cout << "No selection made. Press Enter to exit." << std::endl;
                        break;
                    }

                    // Parse and process the input for single numbers or ranges
                    std::istringstream iss(input);
                    int start, end;
                    char dash;
                    if (iss >> start) {
                        if (iss >> dash && dash == '-' && iss >> end) {
                            // Range input (e.g., 1-5)
                            if (start >= 1 && start <= binImgFiles.size() && end >= start && end <= binImgFiles.size()) {
                                for (int i = start; i <= end; i++) {
                                    selectedFiles.push_back(binImgFiles[i - 1]);
                                }
                            } else {
                                std::cout << "Invalid range. Please try again." << std::endl;
                            }
                        } else if (start >= 1 && start <= binImgFiles.size()) {
                            // Single number input
                            selectedFiles.push_back(binImgFiles[start - 1]);
                        } else {
                            std::cout << "Invalid number. Please try again." << std::endl;
                        }
                    } else {
                        std::cout << "Invalid input format. Please try again." << std::endl;
                    }
                }

                //if (!selectedFiles.empty()) {
                  //  processSelectedFiles(selectedFiles);
                //}
            }
        } else if (choice == "5") {
            // Call your listMountedISOs function
            listMountedISOs();
            std::cout << "Press Enter to continue...";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::system("clear");
        } else if (choice == "6") {
            std::cout << "Exiting the program..." << std::endl;
            return 0;
        } else {
            std::cout << "Invalid choice. Please enter 1, 2, 3, 4, 5, or 6." << std::endl;
        }
    }

    return 0;
}

// ... Function definitions ...


void print_ascii() {
    /// Display ASCII art

std::cout << "\033[32m  _____          ___ _____ _____   ___ __   __  ___  _____ _____   __   __  ___ __   __ _   _ _____ _____ ____          _   ___   ___             \033[0m" << std::endl;
std::cout << "\033[32m |  ___)   /\\   (   |_   _)  ___) (   )  \\ /  |/ _ \\|  ___)  ___) |  \\ /  |/ _ (_ \\ / _) \\ | (_   _)  ___)  _ \\        / | /   \\ / _ \\  \033[0m" << std::endl;
std::cout << "\033[32m | |_     /  \\   | |  | | | |_     | ||   v   | |_| | |   | |_    |   v   | | | |\\ v / |  \\| | | | | |_  | |_) )  _  __- | \\ O /| | | |      \033[0m" << std::endl;
std::cout << "\033[32m |  _)   / /\\ \\  | |  | | |  _)    | || |\\_/| |  _  | |   |  _)   | |\\_/| | | | | | |  |     | | | |  _) |  __/  | |/ /| | / _ \\| | | |     \033[0m" << std::endl;
std::cout << "\033[32m | |___ / /  \\ \\ | |  | | | |___   | || |   | | | | | |   | |___  | |   | | |_| | | |  | |\\  | | | | |___| |     | / / | |( (_) ) |_| |       \033[0m" << std::endl;
std::cout << "\033[32m |_____)_/    \\_(___) |_| |_____) (___)_|   |_|_| |_|_|   |_____) |_|   |_|\\___/  |_|  |_| \\_| |_| |_____)_|     |__/  |_(_)___/ \\___/       \033[0m" << std::endl;
		
std::cout << " " << std::endl;
}



void unmountISOs() {
    const std::string isoPath = "/mnt";

    while (true) {
        std::vector<std::string> isoDirs;

        // Find and store directories with the name "iso_*" in /mnt
        DIR* dir;
        struct dirent* entry;

        if ((dir = opendir(isoPath.c_str())) != NULL) {
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_type == DT_DIR && std::string(entry->d_name).find("iso_") == 0) {
                    isoDirs.push_back(isoPath + "/" + entry->d_name);
                }
            }
            closedir(dir);
        }

        if (isoDirs.empty()) {
            std::cout << "\033[31mNO ISOS MOUNTED, NOTHING TO DO.\n\033[0m";
            return;
        }

        // Display a list of mounted ISOs with indices
        std::cout << "List of mounted ISOs:" << std::endl;
        for (size_t i = 0; i < isoDirs.size(); ++i) {
            std::cout << i + 1 << ". " << isoDirs[i] << std::endl;
        }

        // Prompt for unmounting input
        std::cout << "\033[33mEnter the range of ISOs to unmount (e.g., 1, 1-3, 1 to 3) or type 'exit' to cancel:\033[0m ";
        std::string input;
        std::getline(std::cin, input);

        if (input == "exit") {
            std::cout << "Exiting the unmounting tool." << std::endl;
            break;  // Exit the loop
        }

        // Continue with the rest of the logic to process user input
        std::istringstream iss(input);
        int startRange, endRange;
        char hyphen;

        if ((iss >> startRange) && (iss >> hyphen) && (iss >> endRange)) {
            if (hyphen == '-' && startRange >= 1 && endRange >= startRange && static_cast<size_t>(endRange) <= isoDirs.size()) {
                // Valid range input
            } else {
                std::cerr << "\033[31mInvalid range. Please try again.\n\033[0m" << std::endl;
                continue;  // Restart the loop
            }
        } else {
            // If no hyphen is present or parsing fails, treat it as a single choice
            endRange = startRange;
        }
        if (startRange < 1 || endRange > static_cast<int>(isoDirs.size()) || startRange > endRange) {
            std::cerr << "\033[31mInvalid range or choice. Please try again.\n\033[0m" << std::endl;
            continue;  // Restart the loop
        }

        // Unmount and attempt to remove the selected range of ISOs, suppressing output
        for (int i = startRange - 1; i < endRange; ++i) {
            const std::string& isoDir = isoDirs[i];

            // Unmount the ISO and suppress logs
            std::string unmountCommand = "sudo umount -l \"" + isoDir + "\" > /dev/null 2>&1";
            int result = system(unmountCommand.c_str());

            if (result == 0) {
                // Omitted log for unmounting
            } else {
                // Omitted log for unmounting
            }

            // Remove the directory, regardless of unmount success, and suppress error message
            std::string removeDirCommand = "sudo rmdir -p \"" + isoDir + "\" 2>/dev/null";
            int removeDirResult = system(removeDirCommand.c_str());

            if (removeDirResult == 0) {
                // Omitted log for directory removal
            }
        }
    }
}



void unmountAndCleanISO(const std::string& isoDir) {
    std::string unmountCommand = "sudo umount -l \"" + isoDir + "\" 2>/dev/null";
    int result = std::system(unmountCommand.c_str());

    // if (result != 0) {
       // std::cerr << "Failed to unmount " << isoDir << " with sudo." << std::endl;
    // }

    // Remove the directory after unmounting
    std::string removeDirCommand = "sudo rmdir \"" + isoDir + "\"";
    int removeDirResult = std::system(removeDirCommand.c_str());

    if (removeDirResult != 0) {
        std::cerr << "Failed to remove directory " << isoDir << std::endl;
    }
}

void cleanAndUnmountISO(const std::string& isoDir) {
    std::lock_guard<std::mutex> lock(mtx);
    unmountAndCleanISO(isoDir);
}

void cleanAndUnmountAllISOs() {
	std::cout << "\n";
    std::cout << "Clean and Unmount All ISOs function." << std::endl;
    const std::string isoPath = "/mnt";
    std::vector<std::string> isoDirs;

    DIR* dir;
    struct dirent* entry;

    if ((dir = opendir(isoPath.c_str())) != NULL) {
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_DIR && std::string(entry->d_name).find("iso_") == 0) {
                isoDirs.push_back(isoPath + "/" + entry->d_name);
            }
        }
        closedir(dir);
    }

    if (isoDirs.empty()) {
        std::cout << "\033[31mNO ISOS TO BE CLEANED\n\033[0m" << std::endl;
        return;
    }

    std::vector<std::thread> threads;

    for (const std::string& isoDir : isoDirs) {
        threads.emplace_back(cleanAndUnmountISO, isoDir);
        if (threads.size() >= 4) {
            for (std::thread& thread : threads) {
                thread.join();
            }
            threads.clear();
        }
    }

    for (std::thread& thread : threads) {
        thread.join();
    }

    std::cout << "\033[32mALL ISOS CLEANED\n\033[0m" << std::endl;
}




void convertBINsToISOs() {
    std::cout << "Convert BINs/IMGs to ISOs function." << std::endl;
    // Implement the logic for this function.
}

void listMountedISOs() {
    std::string path = "/mnt";
    int isoCount = 0;

    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.is_directory() && entry.path().filename().string().find("iso") == 0) {
            // Print the directory number, name, and color.
            std::cout << "\033[1;35m" << ++isoCount << ". " << entry.path().filename().string() << "\033[0m" << std::endl;
        }
    }

    if (isoCount == 0) {
        std::cout << "\033[31mNo ISO(s) mounted.\n\033[0m" << std::endl;
    }
}

void listMode() {
    std::cout << "List Mode selected. Implement your logic here." << std::endl;
    // You can call select_and_mount_files_by_number or add your specific logic.
}

void manualMode_isos() {
    std::cout << "Manual Mode selected. Implement your logic here." << std::endl;
    // You can call manual_mode_isos or add your specific logic.
}

void manualMode_imgs() {
    std::cout << "Manual Mode selected. Implement your logic here." << std::endl;
    // You can call manual_mode_imgs or add your specific logic.
}

void select_and_mount_files_by_number() {
    std::cout << "List and mount files by number. Implement your logic here." << std::endl;
    // Implement the logic for selecting and mountin files.
}

void select_and_convert_files_to_iso() {
    std::cout << "List and mount files by number. Implement your logic here." << std::endl;
    // Implement the logic for selecting and converting files.
}
