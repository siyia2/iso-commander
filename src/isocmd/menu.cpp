// SPDX-License-Identifier: GPL-2.0-or-later

#include "../headers.h"


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


// Function to clear scrollbuffer
void clearScrollBuffer() {
        std::cout << "\033[3J\033[2J\033[H\033[0m" << std::flush;
}
