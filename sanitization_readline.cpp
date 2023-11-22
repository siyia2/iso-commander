#include "sanitization_readline.h"


//	SANITISATION AND STRING STUFF	\\

std::string shell_escape(const std::string& s) {
    // Estimate the maximum size of the escaped string
    size_t max_size = s.size() * 4;  // Assuming every character might need escaping
    std::string escaped_string;
    escaped_string.reserve(max_size);

    for (char c : s) {
        if (c == '\'') {
            escaped_string += "'\\''";
        } else {
            escaped_string += c;
        }
    }

    return "'" + escaped_string + "'";
}

// Function to read a line of input using readline
std::string readInputLine(const std::string& prompt) {
    char* input = readline(prompt.c_str());

    if (input) {
        std::string inputStr(input);
        free(input); // Free the allocated memory for the input
        return inputStr;
    }

    return ""; // Return an empty string if readline fails
}
