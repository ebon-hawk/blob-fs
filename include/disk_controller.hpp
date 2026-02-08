#ifndef DISK_CONTROLLER_HPP
#define DISK_CONTROLLER_HPP

#include <fstream>
#include <list>
#include <unordered_map>
#include <vector>

#include "specs.hpp"

class DiskController {
public:
    // --- INITIALIZATION ---
    static Specs::Superblock initNewDisk(std::fstream& fs, uint64_t maxSize);

    void loadBitmaps(std::fstream& fs, const Specs::Superblock& sb);

    // --- INODE MANAGEMENT ---
    Specs::Inode getInode(std::fstream& fs, const Specs::Superblock& sb, int32_t idx);

    int32_t copyInode(std::fstream& fs, const Specs::Superblock& sb,
        int32_t srcIdx, int32_t parentIdx);

    void freeInode(std::fstream& fs, const Specs::Superblock& sb, int32_t idx);

    void updateInode(std::fstream& fs, const Specs::Superblock& sb,
        int32_t idx, const Specs::Inode& node);

    // --- BITMAP OPERATIONS ---
    int32_t allocateBlock(std::fstream& fs, const Specs::Superblock& sb);

    int32_t allocateInode(std::fstream& fs, const Specs::Superblock& sb);

    void sync(std::fstream& fs, const Specs::Superblock& sb);

    // --- I/O ---
    void readBlock(std::fstream& fs, const Specs::Superblock& sb,
        int32_t blockIdx, char* buffer);

    void writeBlock(std::fstream& fs, const Specs::Superblock& sb,
        int32_t blockIdx, const char* buffer);

private:
    int32_t findFreeBit(const std::vector<uint8_t>& bitmap, uint32_t totalCount);

    void enforceCacheLimit(std::fstream& fs, const Specs::Superblock& sb);

    void freeBlock(int32_t blockIdx);

    // --- RAW DISK I/O ---
    Specs::Inode readInode(std::fstream& fs, const Specs::Superblock& sb, int32_t idx);

    void writeInode(std::fstream& fs, const Specs::Superblock& sb,
        int32_t idx, const Specs::Inode& node);

private:
    static const size_t CACHE_CAPACITY;

    struct CacheEntry {
        Specs::Inode node;
        std::list<int32_t>::iterator it;

        // Track if memory differs from disk
        bool dirty = false;
    };

    std::unordered_map<int32_t, CacheEntry> inodeCache;
    std::vector<uint8_t> dataBitmap;
    std::vector<uint8_t> inodeBitmap;

    std::list<int32_t> lruList;

    bool dataBitmapDirty = false;
    bool inodeBitmapDirty = false;
};

#endif // DISK_CONTROLLER_HPP