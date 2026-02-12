#ifndef STRING_UTILS_HPP
#define STRING_UTILS_HPP

#include <string>
#include <vector>

namespace StringUtils {
    /**
    * Matches a string against a pattern containing '?' and '*' wildcards.
    * This is a "Greedy with Backtracking" implementation. It is significantly
    * more memory-efficient than Dynamic Programming solutions as it uses
    * O(1) extra space.
    *
    * Wildcards:
    * '?' - Matches any single character.
    * '*' - Matches any sequence of characters (including empty).
    *
    * @see https://leetcode.com/problems/wildcard-matching/
    * @param pattern The wildcard pattern (e.g., "data_*.bin")
    * @param text The string to be checked (e.g., "data_001.bin")
    * @return True if the text matches the pattern, false otherwise.
    */
    bool matchesPattern(const std::string& pattern, const std::string& text);

    // Splits "a/b/c" into ["a", "b", "c"]
    std::vector<std::string> tokenize(const std::string& input, char delimiter = '/');
}

#endif