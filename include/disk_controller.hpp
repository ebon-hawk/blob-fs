#ifndef DISK_CONTROLLER_HPP
#define DISK_CONTROLLER_HPP

#include <cstdint>
#include <fstream>
#include <vector>

#include "lru_cache.hpp"
#include "specs.hpp"

// Wrapper for cached inodes to track "dirty" (unsaved) state
struct InodeEntry {
    Specs::Inode node;
    bool dirty = false;
};

class DiskController : public ICacheEventHandler<int32_t, InodeEntry> {
public:
    DiskController(std::fstream& fs, const Specs::Superblock& sb);
    virtual ~DiskController();

    // --- PERSISTENCE CONTROL ---
    void sync();

    // --- RESOURCE ALLOCATION ---
    int32_t allocateBlock();
    int32_t allocateInode();
    int32_t createInode(int32_t parentIdx, const std::string& name, bool isDirectory);
    void freeInode(int32_t inodeIdx);

    // --- INODE ACCESS & MANIPULATION ---
    InodeEntry* getInode(int32_t inodeIdx);
    int32_t copyInode(int32_t sourceIdx, int32_t parentIdx, const std::string& newName = "");
    void updateInode(int32_t inodeIdx, const Specs::Inode& node);

    // --- BLOCK I/O ---
    void readBlock(int32_t blockIdx, char* buffer);
    void writeBlock(int32_t blockIdx, const char* buffer);

    // --- CACHE EVENT HANDLER IMPLEMENTATION ---
    bool onEvict(const int32_t& key, InodeEntry& value) override;

    // --- FILE GROWTH ---
    bool canGrowFile(const Specs::Inode& node, uint32_t bytesToAdd);
    void extendInode(Specs::Inode& node, uint32_t newTotalSize);
    void linkToIndirect(Specs::Inode& node, uint32_t pos, int32_t blockIdx);

    uint32_t getFreeDiskBlocks() const;
    uint32_t maxBlocksPerInode() const;
    uint64_t maxFileSizeBytes() const;

    // --- DEEP COPY ---
    void copyData(const Specs::Inode& src, Specs::Inode& dest);
    void copyIndirectData(int32_t srcIdx, int32_t& destIdx);
    void freeData(Specs::Inode& node);
    void freeIndirectData(int32_t idx);

private:
    void clearInodeCacheDirtyFlags();
    void syncInodeCache();

    // --- DISK I/O ---
    uint32_t getBlockOffset(int32_t blockIdx) const;
    uint32_t getInodeOffset(int32_t inodeIdx) const;

    Specs::Inode readInodeFromDisk(int32_t inodeIdx);
    void writeInodeToDisk(int32_t inodeIdx, const Specs::Inode& node);

    // --- BITMAP MANAGEMENT ---
    int32_t allocateResource(std::vector<uint8_t>& bitmap, uint32_t totalCount, uint32_t& hint, bool& dirtyFlag);
    int32_t findFreeBit(const std::vector<uint8_t>& bitmap, uint32_t totalCount, uint32_t& hint);
    void freeBlock(int32_t blockIdx);
    void loadBitmaps();
    void toggleBit(std::vector<uint8_t>& bitmap, int32_t idx, bool set);

private:
    static const size_t INODE_CACHE_CAPACITY;

    std::fstream& fs;

    const Specs::Superblock& sb;

    LRUCache<int32_t, InodeEntry> inodeCache;

    std::vector<uint8_t> blockBitmap;
    std::vector<uint8_t> inodeBitmap;

    uint32_t lastBlockHint = 0;
    uint32_t lastInodeHint = 0;

    bool blockBitmapDirty = false;
    bool inodeBitmapDirty = false;
};

#endif