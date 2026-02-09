#ifndef STRING_UTILS_HPP
#define STRING_UTILS_HPP

#include <string>
#include <vector>

namespace StringUtils {
    bool matchesPattern(const std::string& pattern, const std::string& text);

    // Splits "a/b/c" into ["a", "b", "c"]
    std::vector<std::string> tokenize(const std::string& input, char delimiter = '/');
}

#endif // STRING_UTILS_HPP