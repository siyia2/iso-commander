// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../display.h"


// Canonical ordered defaults for all config keys
static const std::vector<std::pair<std::string, std::string>> CONFIG_ORDERED_DEFAULTS = {
    {"auto_update",      "off"},
    {"filenames_only",   "off"},
    {"pagination",       "25"},
    {"mount_list",       "compact"},
    {"umount_list",      "full"},
    {"cp_mv_rm_list",    "compact"},
    {"write_list",       "compact"},
    {"conversion_lists", "compact"},
    {"max_thread_cap",      "32"},
    {"threads_for_cp_mv",     "8"},
    {"threads_for_conversions",     "8"},
    {"threads_for_mount",    "16"},
    {"threads_for_umount",    "16"},
    {"threads_for_mount",   "32"},
    {"threads_for_rm",       "32"},
    {"threads_for_list_sorting",     "4"},
    {"threads_for_list_filtering",   "4"}
};


// Function to read a configuration file and store key-value pairs in a map
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
                std::string key   = line.substr(0, equalPos);
                std::string value = line.substr(equalPos + 1);
                config[trim(key)] = trim(value);
            }
        }
        inFile.close();
    }

    return config;
}


// Helper to write config in canonical order, merging current values with defaults
static void writeConfig(const std::string& configPath,
                        const std::map<std::string, std::string>& config) {
    std::ofstream outFile(configPath);
    if (!outFile) return;
    for (const auto& [key, def] : CONFIG_ORDERED_DEFAULTS) {
        auto it = config.find(key);
        outFile << key << " = " << (it != config.end() ? it->second : def) << "\n";
    }
}


// Helper to ensure all default keys are present, writing file if anything was missing.
// Returns true if the map was already complete, false if defaults were injected.
static bool ensureDefaults(std::map<std::string, std::string>& configMap,
                           const std::string& configPath) {
    bool needsUpdate = false;
    for (const auto& [key, def] : CONFIG_ORDERED_DEFAULTS) {
        if (configMap.find(key) == configMap.end()) {
            configMap[key] = def;
            needsUpdate = true;
        }
    }
    if (needsUpdate) writeConfig(configPath, configMap);
    return !needsUpdate;
}


// Helper to apply thread caps from a fully-populated config map
static void applyThreadCaps(const std::map<std::string, std::string>& configMap) {
    auto getThreadVal = [&](const std::string& key, size_t defaultVal) -> size_t {
        auto it = configMap.find(key);
        if (it == configMap.end()) return defaultVal;
        try {
            int v = std::stoi(it->second);
            return (v > 0) ? static_cast<size_t>(v) : defaultVal;
        } catch (...) {
            return defaultVal;
        }
    };

    MAX_USEFUL_THREADS = getThreadVal("max_thread_cap",    32);
    CPMV_THREAD_CAP    = getThreadVal("threads_for_cp_mv",    8);
    CONV_THREAD_CAP    = getThreadVal("threads_for_conversions",    8);
    MOUNT_THREAD_CAP   = getThreadVal("threads_for_mount",  16);
    CLEAN_THREAD_CAP   = getThreadVal("threads_for_umount",  16);
    UMOUNT_THREAD_CAP  = getThreadVal("threads_for_mount", 32);
    RM_THREAD_CAP      = getThreadVal("threads_for_rm",     32);
    SORT_THREAD_CAP    = getThreadVal("threads_for_list_sorting",    4);
    FILTER_THREAD_CAP  = getThreadVal("threads_for_list_filtering",  4);
}


// Function to get AutomaticImportConfig status
bool readUserConfigUpdates(const std::string& filePath) {
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

        std::string key      = line.substr(0, equalsPos);
        std::string valueStr = line.substr(equalsPos + 1);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        valueStr.erase(0, valueStr.find_first_not_of(" \t"));
        valueStr.erase(valueStr.find_last_not_of(" \t") + 1);

        for (const auto& pair : CONFIG_ORDERED_DEFAULTS) {
            if (key == pair.first) {
                configMap[key] = valueStr;
                break;
            }
        }
    }
    inFile.close();

    ensureDefaults(configMap, filePath);

    return (configMap["auto_update"] == "on");
}


// Function to read the configuration file and set pagination settings
bool paginationSet(const std::string& filePath) {
    std::ifstream inFile(filePath);
    if (!inFile) return false;

    std::string line;
    while (std::getline(inFile, line)) {
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);
        if (line.empty() || line[0] == '#') continue;

        size_t equalsPos = line.find('=');
        if (equalsPos == std::string::npos) continue;

        std::string key      = line.substr(0, equalsPos);
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

    // Ensure the parent directory exists
    fs::path configFilePath(filePath);
    if (!fs::exists(configFilePath.parent_path()) && !configFilePath.parent_path().empty()) {
        fs::create_directories(configFilePath.parent_path());
    }

    std::ifstream inFile(filePath);

    // If the file cannot be opened, create and write defaults
    if (!inFile) {
        for (const auto& [key, def] : CONFIG_ORDERED_DEFAULTS)
            configMap[key] = def;
        writeConfig(filePath, configMap);
        // Apply display flags and thread caps from defaults then return
        displayConfig::toggleFullListMount       = false;
        displayConfig::toggleFullListUmount      = true;   // umount default is "full"
        displayConfig::toggleFullListCpMvRm      = false;
        displayConfig::toggleFullListWrite       = false;
        displayConfig::toggleFullListConversions = false;
        displayConfig::toggleNamesOnly           = false;
        applyThreadCaps(configMap);
        return configMap;
    }

    // Read existing config
    std::string line;
    while (std::getline(inFile, line)) {
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);
        if (line.empty() || line[0] == '#') continue;

        size_t equalsPos = line.find('=');
        if (equalsPos == std::string::npos) continue;

        std::string key      = line.substr(0, equalsPos);
        std::string valueStr = line.substr(equalsPos + 1);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        valueStr.erase(0, valueStr.find_first_not_of(" \t"));
        valueStr.erase(valueStr.find_last_not_of(" \t") + 1);

        for (const auto& pair : CONFIG_ORDERED_DEFAULTS) {
            if (key == pair.first) {
                configMap[key] = valueStr;
                break;
            }
        }
    }
    inFile.close();

    ensureDefaults(configMap, filePath);

    // Set display flags
    displayConfig::toggleFullListMount       = (configMap["mount_list"]       == "full");
    displayConfig::toggleFullListUmount      = (configMap["umount_list"]      == "full");
    displayConfig::toggleFullListCpMvRm      = (configMap["cp_mv_rm_list"]    == "full");
    displayConfig::toggleFullListWrite       = (configMap["write_list"]       == "full");
    displayConfig::toggleFullListConversions = (configMap["conversion_lists"] == "full");
    displayConfig::toggleNamesOnly           = (configMap["filenames_only"]   == "on");

    // Apply thread caps
    applyThreadCaps(configMap);

    return configMap;
}


// Function to write number of entries per page for pagination
void updatePagination(const std::string& inputSearch, const std::string& configPath) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

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
        paginationValueStr = inputSearch.substr(12);
        paginationValue    = std::stoi(paginationValueStr);
    } catch (const std::invalid_argument&) {
        std::cerr << "\n\033[1;91mInvalid pagination value: '\033[1;93m"
                  << paginationValueStr << "\033[1;91m' is not a valid number.\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }

    std::map<std::string, std::string> config = readConfig(configPath);
    config["pagination"] = std::to_string(paginationValue);

    writeConfig(configPath, config);
    ITEMS_PER_PAGE = paginationValue;

    if (paginationValue > 0) {
        std::cout << "\n\033[0;1mPagination status updated: Max entries per page set to \033[1;93m"
                  << paginationValue << "\033[1;97m.\033[0m\n";
    } else {
        std::cout << "\n\033[0;1mPagination status updated: \033[1;91mDisabled\033[0;1m.\n";
    }

    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Function to set the FilenamesOnly switch in the config file
void updateFilenamesOnly(const std::string& configPath, const std::string& inputSearch) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

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

    std::map<std::string, std::string> config = readConfig(configPath);

    if (inputSearch == "*flno_on") {
        config["filenames_only"]         = "on";
        displayConfig::toggleNamesOnly   = true;
    } else if (inputSearch == "*flno_off") {
        config["filenames_only"]         = "off";
        displayConfig::toggleNamesOnly   = false;
    }

    std::ofstream outFile(configPath);
    if (outFile.is_open()) {
        writeConfig(configPath, config);
        if (inputSearch == "*flno_on" || inputSearch == "*flno_off") {
            std::cout << "\n\033[0;1mFilename-only lists have been "
                      << (inputSearch == "*flno_on" ? "\033[1;92menabled" : "\033[1;91mdisabled")
                      << "\033[0;1m.\033[0;1m\n";
        }
    } else {
        std::cerr << "\n\033[1;91mError: Unable to access configuration file: \033[1;93m'"
                  << configPath << "'\033[1;91m.\033[0;1m\n";
    }

    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Hold valid input for list display mode commands
const std::unordered_map<char, std::string> settingMap = {
    {'m', "mount_list"},
    {'u', "umount_list"},
    {'o', "cp_mv_rm_list"},
    {'c', "conversion_lists"},
    {'w', "write_list"}
};


// Function to validate list display mode input dynamically
bool isValidInput(const std::string& input) {
    if (input.size() < 4 || input[0] != '*' ||
        (input.substr(1, 2) != "cl" && input.substr(1, 2) != "fl")) {
        return false;
    }

    size_t underscorePos = input.find('_', 3);
    if (underscorePos == std::string::npos || underscorePos + 1 >= input.size()) {
        return false;
    }

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
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    std::vector<std::string> settingKeys;
    bool validInput = true;
    std::string newValue;

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
        std::string command      = inputSearch.substr(1, 2);
        size_t underscorePos     = inputSearch.find('_', 3);
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
                    if (uniqueKeys.insert(it->second).second)
                        settingKeys.push_back(it->second);
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

    // Read current config, apply changes, write back
    std::map<std::string, std::string> config = readConfig(configPath);
    for (const auto& key : settingKeys)
        config[key] = newValue;
    writeConfig(configPath, config);

    // Update toggle flags
    for (const auto& key : settingKeys) {
        if      (key == "mount_list")       displayConfig::toggleFullListMount       = (newValue == "full");
        else if (key == "umount_list")      displayConfig::toggleFullListUmount      = (newValue == "full");
        else if (key == "cp_mv_rm_list")    displayConfig::toggleFullListCpMvRm      = (newValue == "full");
        else if (key == "conversion_lists") displayConfig::toggleFullListConversions = (newValue == "full");
        else if (key == "write_list")       displayConfig::toggleFullListWrite       = (newValue == "full");
    }

    std::cout << "\n\033[0;1mDisplay mode set to \033[1;92m" << newValue << "\033[0;1m for:\n";
    for (const auto& key : settingKeys)
        std::cout << "  - " << key << "\n";
    std::cout << "\033[0;1m";

    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Function to update a single thread cap from user input.
// Expected format: "*threads_<name>=<value>"  e.g. "*threads_cpmv=4"
void updateThreadCap(const std::string& inputSearch, const std::string& configPath) {
    signal(SIGINT, SIG_IGN);
    disable_ctrl_d();

    static const std::unordered_map<std::string, std::string> threadKeyMap = {
        {"max",    "max_thread_cap"},
        {"cpmv",   "threads_for_cp_mv"},
        {"conv",   "threads_for_conversions"},
        {"mount",  "threads_for_mount"},
        {"clean",  "threads_for_umount"},
        {"umount", "threads_for_mount"},
        {"rm",     "threads_for_rm"},
        {"sort",   "threads_for_list_sorting"},
        {"filter", "threads_for_list_filtering"}
    };

    const std::string prefix = "*threads_";
    if (inputSearch.substr(0, prefix.size()) != prefix) {
        std::cerr << "\n\033[1;91mInvalid format. Use: *threads_<name>=<value>\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }

    std::string rest    = inputSearch.substr(prefix.size());
    size_t eqPos        = rest.find('=');
    if (eqPos == std::string::npos) {
        std::cerr << "\n\033[1;91mExpected '=' in input (e.g. *threads_cpmv=4).\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }

    std::string shortName = rest.substr(0, eqPos);
    std::string valueStr  = rest.substr(eqPos + 1);

    auto it = threadKeyMap.find(shortName);
    if (it == threadKeyMap.end()) {
        std::cerr << "\n\033[1;91mUnknown thread cap name: '\033[1;93m"
                  << shortName << "\033[1;91m'.\033[0;1m\n"
                  << "\033[0;1mValid names: max, cpmv, conv, mount, clean, umount, rm, sort, filter\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }
    const std::string& configKey = it->second;

    int newVal = 0;
    try {
        newVal = std::stoi(valueStr);
        if (newVal <= 0) throw std::out_of_range("non-positive");
    } catch (...) {
        std::cerr << "\n\033[1;91mInvalid value: '\033[1;93m"
                  << valueStr << "\033[1;91m' — must be a positive integer.\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return;
    }

    // Ensure directory exists
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

    std::map<std::string, std::string> config = readConfig(configPath);
    config[configKey] = std::to_string(newVal);
    writeConfig(configPath, config);
    applyThreadCaps(config);

    std::cout << "\n\033[0;1mThread cap '\033[1;93m" << configKey
              << "\033[0;1m' updated to \033[1;93m" << newVal << "\033[0;1m.\033[0m\n";
    std::cout << "\n\033[1;32m↵ to continue...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}


// Function to read and display configuration options from config file
void displayConfigurationOptions(const std::string& configPath) {
    clearScrollBuffer();

    auto reportError = [&](const std::string& msg) {
        std::cerr << "\n\033[1;91m" << msg << "\033[1;91m.\033[0;1m\n";
        std::cout << "\n\033[1;32m↵ to return...\033[0;1m";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    };

    auto createDefaultConfig = [&]() -> bool {
        std::filesystem::path configDir = std::filesystem::path(configPath).parent_path();
        if (!configDir.empty() && !std::filesystem::exists(configDir)) {
            try {
                std::filesystem::create_directories(configDir);
            } catch (const std::filesystem::filesystem_error&) {
                reportError("Unable to access configuration file: \033[1;93m'" + configPath + "'");
                return false;
            }
        }

        std::ofstream newConfigFile(configPath);
        if (!newConfigFile.is_open()) {
            reportError("Unable to access configuration file: \033[1;93m'" + configPath + "'");
            return false;
        }
        for (const auto& [key, def] : CONFIG_ORDERED_DEFAULTS)
            newConfigFile << key << " = " << def << "\n";
        newConfigFile.close();
        return true;
    };

    std::ifstream configFile(configPath);
    if (!configFile.is_open()) {
        if (!createDefaultConfig()) return;
        configFile.open(configPath);
        if (!configFile.is_open()) {
            reportError("Unable to access configuration file: \033[1;93m'" + configPath + "'");
            return;
        }
    }

    std::cout << "\n\033[1;96m==== Configuration Options ====\033[0;1m\n\n";
    std::string line;
    int lineNumber = 1;
    while (std::getline(configFile, line)) {
        if (!line.empty() && line[0] != '#') {
            std::cout << "\033[1;92m" << lineNumber++ << ". \033[1;97m"
                      << line << "\033[0m\n";
        }
    }
    configFile.close();

    std::cout << "\n\033[1;93mConfiguration file: \033[1;97m"
              << configPath << "\033[0;1m\n";
    std::cout << "\n\033[1;32m↵ to return...\033[0;1m";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}
