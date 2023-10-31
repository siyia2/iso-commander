#include <iostream>
#include <unordered_map>
#include <string>
#include <readline/readline.h>
#include <readline/history.h>
#include <fstream>

// Define the default cache directory
const std::string cacheDirectory = "/tmp/";

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

int main() {
    using_history();

    // Display ASCII art
std::cout << " _____          ___ _____ _____   ___ __   __  ___  _____ _____   __   __  ___ __   __ _   _ _____ _____ ____          _   ___   ___  " << std::endl;
        std::cout << "|  ___)   /\\   (   |_   _)  ___) (   )  \\ /  |/ _ \\|  ___)  ___) |  \\ /  |/ _ (_ \\ / _) \\ | (_   _)  ___)  _ \\        / | /   \\ / _ \\ " << std::endl;
        std::cout << "| |_     /  \\   | |  | | | |_     | ||   v   | |_| | |   | |_    |   v   | | | |\\ v / |  \\| | | | | |_  | |_) )  _  __- | \\ O /| | | |" << std::endl;
        std::cout << "|  _)   / /\\ \\  | |  | | |  _)    | || |\\_/| |  _  | |   |  _)   | |\\_/| | | | | | |  |     | | | |  _) |  __/  | |/ /| | / _ \\| | | |" << std::endl;
        std::cout << "| |___ / /  \\ \\ | |  | | | |___   | || |   | | | | | |   | |___  | |   | | |_| | | |  | |\\  | | | | |___| |     | / / | |( (_) ) |_| |" << std::endl;
        std::cout << "|_____)_/    \\_(___) |_| |_____) (___)_|   |_|_| |_|_|   |_____) |_|   |_|\\___/  |_|  |_| \\_| |_| |_____)_|     |__/  |_(_)___/ \\___/" << std::endl;
        std::cout << std::endl;
    
    bool returnToMainMenu = false;

    main_menu:
    while (true) {
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
                    break;
                case 3:
                    // Call your cleanAndUnmountAllISOs function
                    cleanAndUnmountAllISOs();
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
                    std::cout << "Returning to the main menu..." << std::endl;
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


void listAndMountISOs() {
    std::cout << "List and Mount ISOs function." << std::endl;
    // Implement the logic for this function.
}

void unmountISOs() {
    std::cout << "Unmount ISOs function." << std::endl;
    // Implement the logic for this function.
}

void cleanAndUnmountAllISOs() {
    std::cout << "Clean and Unmount All ISOs function." << std::endl;
    // Implement the logic for this function.
}

void convertBINsToISOs() {
    std::cout << "Convert BINs/IMGs to ISOs function." << std::endl;
    // Implement the logic for this function.
}

void listMountedISOs() {
    std::cout << "List Mounted ISOs function." << std::endl;
    // Implement the logic for this function.
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
