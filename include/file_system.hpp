#ifndef FILE_SYSTEM_HPP
#define FILE_SYSTEM_HPP

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "disk_controller.hpp"
#include "inode_linker.hpp"
#include "lru_cache.hpp"
#include "path_engine.hpp"
#include "specs.hpp"

class FileSystem {
public:
    FileSystem(std::fstream& fs, const Specs::Superblock& sb);
    ~FileSystem();

    static Specs::Superblock createFresh(std::fstream& fs, uint64_t maxDiskSize);

    std::string getCurrentPath();

    // --- PUBLIC SHELL COMMANDS ---
    void cd(const std::string& path);
    void ls(const std::string& path = "");
    void mkdir(const std::string& path);
    void rmdir(const std::string& path);

    void cat(const std::string& path);
    void cp(const std::string& srcPath, const std::string& destPath);
    void rm(const std::string& path);

    // --- HOST OS INTERACTION ---
    void exportFile(const std::string& srcPath, const std::string& hostDest);
    void importFile(const std::string& hostSrc, const std::string& destPath, bool append = false);

private:
    // --- FILE VALIDATION & HELPERS ---
    bool isValidFilename(const std::string& filename) const;
    int32_t getPhysicalBlock(const Specs::Inode& node, uint32_t logicalIdx);
    void printFileContents(int32_t inodeIdx);
    void updateCurrentDir(int32_t dirIdx);

private:
    static const size_t DIR_CACHE_CAPACITY;

    // --- CORE STORAGE & CACHE ---
    std::fstream& fs;

    Specs::Superblock sb;

    DiskController controller;
    LRUCache<int32_t, Specs::Dentry> dirCache;

    // --- ENGINE COMPONENTS ---
    InodeLinker linker;
    PathEngine pathEngine;

    // --- RUNTIME STATE ---
    int32_t currentDirIdx = 0;
    int32_t prevDirIdx = Specs::NULL_INDEX;
};

#endif