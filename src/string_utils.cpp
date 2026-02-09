#include <sstream>

#include "string_utils.hpp"

bool StringUtils::matchesPattern(const std::string& pattern, const std::string& text) {
    size_t p = pattern.length();
    size_t t = text.length();

    size_t i = 0, j = 0, match = 0, startIndex = std::string::npos;

    while (i < t) {
        // If characters match or pattern has '?'
        if (j < p && (pattern[j] == '?' || pattern[j] == text[i])) {
            ++i;
            ++j;
        }
        // If pattern has '*', mark the position and try to match 0 characters
        else if (j < p && pattern[j] == '*') {
            startIndex = j;

            match = i;

            ++j;
        }
        // If last match was a '*', backtrack and try matching one more character
        else if (startIndex != std::string::npos) {
            j = startIndex + 1;
            match++;

            i = match;
        }
        else {
            return false;
        }
    }

    // Handle trailing '*' in pattern (e.g., "file**")
    while (j < p && pattern[j] == '*') {
        ++j;
    }

    return j == p;
}

std::vector<std::string> StringUtils::tokenize(const std::string& input, char delimiter) {
    std::vector<std::string> tokens;

    std::stringstream ss(input);

    std::string token;

    while (std::getline(ss, token, delimiter)) {
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }

    return tokens;
}