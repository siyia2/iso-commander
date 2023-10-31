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


// Define the default cache directory
const std::string cacheDirectory = "/tmp/";

namespace fs = std::filesystem;

// Define a mutex for synchronization
std::mutex mtx;



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

int main() {
    using_history();
	print_ascii();

    bool returnToMainMenu = false;

    main_menu:
    while (true) {
    // Display ASCII art
    	
        char* choice;
        char* prompt = (char*)"Select an option:\n1) List and Mount ISOs\n2) Unmount ISOs\n3) Clean and Unmount All ISOs\n4) Convert BIN(s)/IMG(s) to ISO(s)\n5) List Mounted ISO(s)\n6) Exit\nEnter the number of your choice: ";
        while ((choice = readline(prompt)) != nullptr) {
            add_history(choice);
            int option = atoi(choice);
            free(choice);

            switch (option) {
                case 1:
                    while (true) {
                        char* choice_small;
                        char* small_prompt = (char*)"\n1) List Mode\n2) Manual Mode\n3) Return to the main menu\nSelect a mode: ";
                        while ((choice_small = readline(small_prompt)) != nullptr) {
                            add_history(choice_small);
                            int small_option = atoi(choice_small);
                            free(choice_small);

                            switch (small_option) {
                                case 1:
                                    // Call your listMode function
                                    listMode();
                                    break;
                                case 2:
                                    // Call your manualMode function
                                    manualMode_isos();
                                    break;
                                case 3:
                                    std::cout << "Returning to the main menu..." << std::endl;
                                    goto main_menu;  // Use a goto statement to return to the main menu
                                default:
                                    std::cout << "Invalid choice. Please enter 1, 2, or 3." << std::endl;
                            }

                            if (small_option == 3) {
                                break;
                            }
                        }
                    }
                    break;
                case 2:
                    // Call your unmountISOs function
                    unmountISOs();
		std::cout << "Press Enter to continue...";
    	     	std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
		std::system("clear");
			print_ascii();
                    break;
                case 3:
                    // Call your cleanAndUnmountAllISOs function
                    cleanAndUnmountAllISOs();
		    // Prompt the user to press Enter to continue
    		    std::cout << "Press Enter to continue...";
    	     	    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
		    std::system("clear");
			print_ascii();
                    break;
                case 4:
    while (true) {
        char* choice_small;
        char* small_prompt = (char*)"\n1) List Mode\n2) Manual Mode\n3) Return to the main menu\nSelect a mode: ";
        while ((choice_small = readline(small_prompt)) != nullptr) {
            add_history(choice_small);
            int small_option = atoi(choice_small);
            free(choice_small);

            switch (small_option) {
                case 1:
                    std::cout << "Operating In List Mode" << std::endl;
                    // Call your select_and_convert_files_to_iso_multithreaded function here
                    select_and_convert_files_to_iso();
                    break;
                case 2:
                    // Call your manual_mode_imgs function here
                    manualMode_imgs();
                    break;
                case 3:                  
                    goto main_menu;  // Use a goto statement to return to the main menu
                default:
                    std::cout << "Error: Invalid choice: " << small_option << std::endl;
            }

            if (small_option == 3) {
                break;
            }
        }
    }
    break;

                case 5:
                    // Call your listMountedISOs function
                    listMountedISOs();
		    std::cout << "Press Enter to continue...";
    	     	    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
		    std::system("clear");
			print_ascii();
                    break;
                case 6:
                    std::cout << "Exiting the program..." << std::endl;
                    return 0;
                default:
                    std::cout << "Invalid choice. Please enter 1, 2, 3, 4, 5, or 6." << std::endl;
            }
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
