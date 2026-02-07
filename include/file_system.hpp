#ifndef FILE_SYSTEM_HPP
#define FILE_SYSTEM_HPP

#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "disk_controller.hpp"
#include "specs.hpp"

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
    // --- NAVIGATION HELPERS ---
    // Finds a single exact match (used during path walking)
    int32_t findChildInDirectory(int32_t dirIdx, const std::string& name);

    // Finds all children matching a wildcard pattern (used for cat, cp, ls, rm)
    std::vector<int32_t> findAllMatches(int32_t dirIdx, const std::string& pattern);

    // Splits "a/b/c" into ["a", "b", "c"]
    std::vector<std::string> tokenize(const std::string& path, char delimiter = '/');

    // --- TREE MANIPULATION HELPERS ---
    void addEntryToDirectory(int32_t parentIdx, int32_t childIdx);
    void deleteInodeRecursive(int32_t idx);
    void unlinkInodeFromParent(int32_t targetIdx);

    // --- DATA HELPERS ---
    bool matchesPattern(const std::string& pattern, const std::string& name);

    // Links a new data block to an inode (handles direct/indirect pointers)
    void attachBlockToInode(Specs::Inode& node, int32_t inodeIdx, int32_t blockIdx);

    // --- EDGE CASE VALIDATORS ---
    bool isDirectoryEmpty(int32_t dirIdx);
    bool isValidFilename(const std::string& name);

private:
    // Cache/bitmap manager
    DiskController controller;

    // Reference to the open disk file
    std::fstream& fs;

    // Cached copy of the superblock
    Specs::Superblock sb;

    // The "state" (where the user is right now)
    int32_t currentDirIdx;
};

#endif // FILE_SYSTEM_HPP