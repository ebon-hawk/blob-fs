#ifndef FILE_SYSTEM_HPP
#define FILE_SYSTEM_HPP

#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "disk_controller.hpp"
#include "lru_cache.hpp"
#include "specs.hpp"

// Directory entry cache: maps filename to inode index
struct Dentry {
    std::unordered_map<std::string, int32_t> nameToInode;
};

class FileSystem {
public:
    FileSystem(std::fstream& file, const Specs::Superblock& superblock);

    // --- DIRECTORY OPERATIONS ---
    void cd(const std::string& path);
    void ls(const std::string& path = "");
    void mkdir(const std::string& path);
    void rmdir(const std::string& path);

    // --- FILE OPERATIONS ---
    void cat(const std::string& pattern);
    void cp(const std::string& sourcePattern, const std::string& destPath);
    void exportFile(const std::string& sourcePath, const std::string& hostDest);
    void importFile(const std::string& hostSource, const std::string& destPath, bool append = false);
    void rm(const std::string& pattern);

    // --- NAVIGATION & MAINTENANCE ---
    int32_t resolvePath(const std::string& path);
    void sync();

private:
    // --- DATA HELPERS ---
    Dentry* getOrPopulateDentry(int32_t dirIdx);
    int32_t findChildInDirectory(int32_t dirIdx, const std::string& name);
    std::string getFullPath(int32_t idx);
    std::vector<int32_t> findAllMatches(int32_t dirIdx, const std::string& pattern);
    void attachBlockToInode(Specs::Inode& node, int32_t inodeIdx, int32_t blockIdx);
    void updateCurrentDir(int32_t newIdx);

    // --- TREE MANIPULATION HELPERS ---
    void addEntryToDirectory(int32_t parentIdx, int32_t childIdx);
    void deleteInodeRecursive(int32_t idx);
    void unlinkInodeFromParent(int32_t targetIdx);

    // --- EDGE CASE VALIDATORS ---
    bool isDirectoryEmpty(int32_t dirIdx);
    bool isValidFilename(const std::string& name);

private:
    static const size_t CACHE_CAPACITY;

    std::fstream& fs;

    Specs::Superblock sb;

    DiskController controller;

    LRUCache<int32_t, Dentry> dirCache;

    int32_t currentDirIdx = 0;

    int32_t prevDirIdx = Specs::NULL_INDEX;
};

#endif // FILE_SYSTEM_HPP