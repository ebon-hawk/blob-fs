#ifndef DRIVER_HPP
#define DRIVER_HPP

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "disk_controller.hpp"
#include "specs.hpp"

namespace Driver {
    // Converts "/home/user/file.txt" into an inode index
    int32_t resolvePath(std::fstream& fs, const Specs::Superblock& sb,
        int32_t currentDirIdx, const std::string& path);

    std::vector<std::string> tokenize(const std::string& path);

    // Pattern matching for wildcards (* and ?)
    bool matchesPattern(const char* pattern, const char* name);

    std::vector<int32_t> findMatchesInDirectory(std::fstream& fs,
        const Specs::Superblock& sb, int32_t dirIdx, const std::string& pattern);

    // --- DIRECTORY OPERATIONS ---
    // High-level commands
    void listDirectory(std::fstream& fs, const Specs::Superblock& sb, int32_t dirIdx);

    void makeDirectory(std::fstream& fs, const Specs::Superblock& sb,
        int32_t currentDirIdx, const std::string& name);

    void removeDirectory(std::fstream& fs, const Specs::Superblock& sb,
        int32_t currentDirIdx, const std::string& path);

    // Tree linking/unlinking helpers
    void addEntryToDirectory(std::fstream& fs, const Specs::Superblock& sb,
        int32_t parentIdx, int32_t childIdx);

    void moveEntry(std::fstream& fs, const Specs::Superblock& sb,
        int32_t currentDirIdx, const std::string& srcPath, const std::string& destPath);

    void unlinkInode(std::fstream& fs, const Specs::Superblock& sb, int32_t targetIdx);

    // --- FILE OPERATIONS ---
    // Content management
    void catFile(std::fstream& fs, const Specs::Superblock& sb,
        int32_t currentDirIdx, const std::string& path);

    void removeFile(std::fstream& fs, const Specs::Superblock& sb,
        int32_t currentDirIdx, const std::string& path);

    void removeFilesWithPattern(std::fstream& fs, const Specs::Superblock& sb,
        int32_t currentDirIdx, const std::string& pattern);

    // Host system bridge (import/export)
    void exportFile(std::fstream& fs, const Specs::Superblock& sb,
        int32_t currentDirIdx, const std::string& sourcePath,
        const std::string& hostDestPath);

    void importFile(std::fstream& fs, Specs::Superblock& sb,
        int32_t currentDirIdx, const std::string& hostSourcePath,
        const std::string& destPath, bool append);

    // Internal block helpers
    int32_t getPhysicalBlockIdx(std::fstream& fs, const Specs::Superblock& sb,
        const Specs::Inode& node, uint32_t logicalBlockIdx);

    void attachBlockToInode(std::fstream& fs, Specs::Superblock& sb,
        Specs::Inode& node, int32_t blockIdx);
}

#endif // DRIVER_HPP