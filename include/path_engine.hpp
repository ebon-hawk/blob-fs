#ifndef PATH_ENGINE_HPP
#define PATH_ENGINE_HPP

#include <string>
#include <vector>

#include "disk_controller.hpp"
#include "lru_cache.hpp"
#include "specs.hpp"

class PathEngine {
public:
    PathEngine(DiskController& controller, LRUCache<int32_t, Specs::Dentry>& dirCache);
    PathEngine(PathEngine&& other) noexcept = default;
    PathEngine(const PathEngine& other) = delete;
    ~PathEngine() = default;

    PathEngine& operator=(PathEngine&& other) noexcept = delete;
    PathEngine& operator=(const PathEngine& other) = delete;

    // Specialized for commands that support wildcards
    Specs::PathQuery parsePathQuery(const std::string& path, int32_t currentDirIdx);

    // Simple child lookup using the dentry cache
    int32_t findChildInDirectory(const std::string& name, int32_t dirIdx);

    // Translates a string (absolute or relative) to an inode index
    int32_t resolvePath(const std::string& path, int32_t currentDirIdx);

    // Reconstructs the string path from an inode
    std::string getFullPath(int32_t inodeIdx);

    // Returns all matching inodes in a directory based on a glob pattern
    std::vector<int32_t> findAllMatches(const std::string& pattern, int32_t dirIdx);

    void splitPath(const std::string& path, std::string& parentPath, std::string& name);

private:
    // Internal helper to ensure a directory's entries are in the dentry cache
    Specs::Dentry* getOrPopulateDentry(int32_t dirIdx);

private:
    DiskController& controller;
    LRUCache<int32_t, Specs::Dentry>& dirCache;
};

#endif