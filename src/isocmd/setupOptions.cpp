// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"


const std::string configPath = std::string(getenv("HOME")) + "/.config/isocmd/config";


// Function to read a configuration file and store key-value pairs in a map
std::map<std::string, std::string> readConfig(const std::string& configPath) {
    // Declare a map to store configuration key-value pairs
    std::map<std::string, std::string> config;
    
    // Open the file at the given configPath
    std::ifstream inFile(configPath);
    
    // Lambda function to trim leading and trailing spaces from a string
    auto trim = [](std::string str) {
        // Remove leading spaces
        str.erase(0, str.find_first_not_of(" "));
        // Remove trailing spaces
        str.erase(str.find_last_not_of(" ") + 1);
        return str;
    };
    
    // Check if the file was successfully opened
    if (inFile.is_open()) {
        std::string line;
        // Read the file line by line
        while (std::getline(inFile, line)) {
            // Find the position of the first '=' character in the line
            size_t equalPos = line.find('=');
            
            // If '=' is found, it indicates a key-value pair
            if (equalPos != std::string::npos) {
                // Extract the key (substring before '=')
                std::string key = line.substr(0, equalPos);
                // Extract the value (substring after '=')
                std::string value = line.substr(equalPos + 1);
                
                // Trim any extra spaces from key and value, then store them in the map
                config[trim(key)] = trim(value);
            }
        }
        // Close the file after reading
        inFile.close();
    }
    
    // Return the populated map
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
// Function to read the configuration file and set pagination settings
bool paginationSet(const std::string& filePath) {
    // Declare a map to store the configuration key-value pairs (unused in this function, but could be used later)
    std::map<std::string, std::string> configMap;

    // Open the file at the given file path
    std::ifstream inFile(filePath);

    // If the file couldn't be opened, return false
    if (!inFile) return false;

    std::string line;
    // Read the file line by line
    while (std::getline(inFile, line)) {
        // Trim leading and trailing whitespace and tabs from the line
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);

        // If the line is empty or starts with a comment ('#'), skip it
        if (line.empty() || line[0] == '#') continue;

        // Find the position of the first '=' character in the line (indicating key-value pair)
        size_t equalsPos = line.find('=');
        // If no '=' is found, skip this line as it doesn't contain a key-value pair
        if (equalsPos == std::string::npos) continue;

        // Extract the key (substring before the '=')
        std::string key = line.substr(0, equalsPos);
        // Extract the value (substring after the '=')
        std::string valueStr = line.substr(equalsPos + 1);

        // Trim leading and trailing whitespace and tabs from both key and value
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        valueStr.erase(0, valueStr.find_first_not_of(" \t"));
        valueStr.erase(valueStr.find_last_not_of(" \t") + 1);

        // If the key is "pagination", try to convert the value to an integer
        if (key == "pagination") {
            try {
                // Attempt to set the ITEMS_PER_PAGE variable with the parsed value
                ITEMS_PER_PAGE = std::stoi(valueStr);
                return true;  // Return true if the value is successfully set
            } catch (...) {
                // If an error occurs during conversion (e.g., invalid format), return false
                return false;
            }
        }
    }

    // Return false if "pagination" key was not found or any error occurred
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


// Hold valid input for general use
const std::unordered_map<char, std::string> settingMap = {
    {'m', "mount_list"},
    {'u', "umount_list"},
    {'o', "cp_mv_rm_list"},
    {'c', "conversion_lists"},
    {'w', "write_list"}
};


// Function to validate input dynamically
bool isValidInput(const std::string& input) {
    // Check if input starts with *cl or *fl
    if (input.size() < 4 || input[0] != '*' || 
        (input.substr(1, 2) != "cl" && input.substr(1, 2) != "fl")) {
        return false;
    }

    // Check for underscore and at least one setting character
    size_t underscorePos = input.find('_', 3);
    if (underscorePos == std::string::npos || underscorePos + 1 >= input.size()) {
        return false;
    }

    // Validate each setting character
    std::string settingsStr = input.substr(underscorePos + 1);
    for (char c : settingsStr) {
        if (settingMap.find(c) == settingMap.end()) {
            return false;
        }
    }

    return true;
}


// Function to write default display modes to config file
void setDisplayMode(const std::string& inputSearch) {
	signal(SIGINT, SIG_IGN);        // Ignore Ctrl+C
	disable_ctrl_d();
    std::vector<std::string> configLines;
    std::vector<std::string> settingKeys;
    bool validInput = true;
    std::string newValue;

    // Read existing config lines
    std::ifstream inFile(configPath);
    if (inFile.is_open()) {
        std::string line;
        while (std::getline(inFile, line)) {
            configLines.push_back(line);
        }
        inFile.close();
    }

    // Create directory if needed
    std::filesystem::path dirPath = std::filesystem::path(configPath).parent_path();
    if (!std::filesystem::exists(dirPath)) {
        if (!std::filesystem::create_directories(dirPath)) {
            std::cerr << "\n\033[1;91mFailed to create directory: \033[1;93m'" 
                      << dirPath.string() << "'\033[1;91m.\033[0;1m\n";
            std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return;
        }
    }

    // Parse input command and settings
    if (inputSearch.size() < 4 || inputSearch[0] != '*' || 
        (inputSearch.substr(1, 2) != "cl" && inputSearch.substr(1, 2) != "fl")) {
        std::cerr << "\n\033[1;91mInvalid input format. Use '*cl' or '*fl' prefix.\033[0;1m\n";
        validInput = false;
    } else {
        std::string command = inputSearch.substr(1, 2);
        size_t underscorePos = inputSearch.find('_', 3);
        if (underscorePos == std::string::npos || underscorePos + 1 >= inputSearch.size()) {
            std::cerr << "\n\033[1;91mExpected '_' followed by settings (e.g., *cl_mu).\033[0;1m\n";
            validInput = false;
        } else {
            std::string settingsStr = inputSearch.substr(underscorePos + 1);
            newValue = (command == "cl") ? "compact" : "full";

            std::unordered_set<std::string> uniqueKeys;
            for (char c : settingsStr) {
                auto it = settingMap.find(c);
                if (it != settingMap.end()) {
                    const std::string& key = it->second;
                    if (uniqueKeys.insert(key).second) {
                        settingKeys.push_back(key);
                    }
                } else {
                    std::cerr << "\n\033[1;91mInvalid setting character: '" << c << "'.\033[0;1m\n";
                    validInput = false;
                    break;
                }
            }
        }
    }

    if (!validInput || settingKeys.empty()) {
        if (validInput) std::cerr << "\n\033[1;91mNo valid settings specified.\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }

    // Update config lines for each setting
    std::unordered_set<std::string> unprocessedSettings(settingKeys.begin(), settingKeys.end());
    for (auto& line : configLines) {
        for (auto it = unprocessedSettings.begin(); it != unprocessedSettings.end();) {
            const std::string& settingKey = *it;
            if (line.find(settingKey + " =") == 0) {
                line = settingKey + " = " + newValue;
                it = unprocessedSettings.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Add new settings if they didn't exist
    for (const auto& settingKey : unprocessedSettings) {
        configLines.push_back(settingKey + " = " + newValue);
    }

    // Write updated config to file
    std::ofstream outFile(configPath);
    if (outFile.is_open()) {
        for (const auto& line : configLines) {
            outFile << line << "\n";
        }
        outFile.close();

        // Update toggle flags for each affected setting
        for (const auto& settingKey : settingKeys) {
            if (settingKey == "mount_list") {
                displayConfig::toggleFullListMount = (newValue == "full");
            } else if (settingKey == "umount_list") {
                displayConfig::toggleFullListUmount = (newValue == "full");
            } else if (settingKey == "cp_mv_rm_list") {
                displayConfig::toggleFullListCpMvRm = (newValue == "full");
            } else if (settingKey == "conversion_lists") {
                displayConfig::toggleFullListConversions = (newValue == "full");
            } else if (settingKey == "write_list") {
                displayConfig::toggleFullListWrite = (newValue == "full");
            }
        }

        // Display confirmation
        std::cout << "\n\033[0;1mDisplay mode set to \033[1;92m" << newValue << "\033[0;1m for:\n";
        for (const auto& key : settingKeys) {
            std::cout << "  - " << key << "\n";
        }
        std::cout << "\033[0;1m";
    } else {
        std::cerr << "\n\033[1;91mError: Unable to access configuration file: \033[1;93m'"
                  << configPath << "'\033[1;91m.\033[0;1m\n";
    }

    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Function to read and display configuration options from config file
void displayConfigurationOptions(const std::string& configPath) {
    clearScrollBuffer();

    // Lambda to report error messages and pause.
    auto reportError = [&](const std::string &msg) {
        std::cerr << "\n\033[1;91m" << msg << "\033[1;91m.\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to return...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    };

    // Lambda to create the default configuration file.
    auto createDefaultConfig = [&]() -> bool {
        std::vector<std::pair<std::string, std::string>> orderedDefaults = {
            {"auto_update", "off"},
            {"pagination", "25"},
            {"mount_list", "compact"},
            {"umount_list", "full"},
            {"cp_mv_rm_list", "compact"},
            {"write_list", "compact"},
            {"conversion_lists", "compact"}
        };

        // Create the directory if it does not exist.
        std::filesystem::path configDir = std::filesystem::path(configPath).parent_path();
        if (!configDir.empty() && !std::filesystem::exists(configDir)) {
            try {
                std::filesystem::create_directories(configDir);
            } catch (const std::filesystem::filesystem_error&) {
                reportError("Unable to access configuration file: \033[1;93m'" + configPath + "'");
                return false;
            }
        }

        // Create and write default values to the config file.
        std::ofstream newConfigFile(configPath);
        if (!newConfigFile.is_open()) {
            reportError("Unable to access configuration file: \033[1;93m'" + configPath + "'");
            return false;
        }
        newConfigFile << "# Default configuration file created on " << configPath << "\n";
        for (const auto& [key, value] : orderedDefaults) {
            newConfigFile << key << "=" << value << "\n";
        }
        newConfigFile.close();
        return true;
    };

    // Try to open the configuration file for reading.
    std::ifstream configFile(configPath);
    if (!configFile.is_open()) {
        // Create default config if the file doesn't exist.
        if (!createDefaultConfig()) {
            return;
        }
        configFile.open(configPath);
        if (!configFile.is_open()) {
            reportError("Unable to access configuration file: \033[1;93m'" + configPath + "'");
            return;
        }
    }

    // Display configuration options.
    std::cout << "\n\033[1;96m==== Configuration Options ====\033[0;1m\n" << std::endl;
    std::string line;
    int lineNumber = 1;
    while (std::getline(configFile, line)) {
        if (!line.empty() && line[0] != '#') {  // Skip comment lines.
            std::cout << "\033[1;92m" << lineNumber++ << ". \033[1;97m" 
                      << line << "\033[0m" << std::endl;
        }
    }
    configFile.close();

    std::cout << "\n\033[1;93mConfiguration file: \033[1;97m" 
              << configPath << "\033[0;1m" << std::endl;
    std::cout << "\n\033[1;32m↵ to return...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}
