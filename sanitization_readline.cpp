#include "sanitization_readline.h"


//	SANITISATION AND STRING STUFF	\\

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

// Function to read a line of input using readline
std::string readInputLine(const std::string& prompt) {
    // Read a line of input using the readline function
    char* input = readline(prompt.c_str());

    // Check if input is not null (readline successful)
    if (input) {
        // Convert the C-style string to a C++ string
        std::string result(input);

        // Free the memory allocated by readline
        free(input);

        // Return the input string
        return result;
    }

    // Return an empty string if readline fails
    return "";
}
