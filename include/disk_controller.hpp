#ifndef DISK_CONTROLLER_HPP
#define DISK_CONTROLLER_HPP

#include <fstream>
#include <vector>

#include "lru_cache.hpp"
#include "specs.hpp"

struct InodeEntry {
    Specs::Inode node;
    bool dirty = false;
};

class DiskController : public ICacheEventHandler<int32_t, InodeEntry> {
public:
    DiskController(std::fstream& fs, const Specs::Superblock& sb);

    virtual ~DiskController();

    // --- INITIALIZATION ---
    void loadBitmaps();

    // --- INODE MANAGEMENT ---
    InodeEntry* getInode(int32_t idx);
    int32_t copyInode(int32_t srcIdx, int32_t parentIdx);
    void freeInode(int32_t idx);
    void updateInode(int32_t idx, const Specs::Inode& node);

    // --- BITMAP OPERATIONS ---
    int32_t allocateBlock();
    int32_t allocateInode();
    void sync();

    // --- I/O ---
    void readBlock(int32_t blockIdx, char* buffer);
    void writeBlock(int32_t blockIdx, const char* buffer);

    // --- INTERFACE IMPLEMENTATION ---
    bool onEvict(const int32_t& key, InodeEntry& value) override;

private:
    int32_t findFreeBit(const std::vector<uint8_t>& bitmap, uint32_t totalCount, uint32_t& hint);
    void freeBlock(int32_t blockIdx);

    // --- RAW DISK I/O ---
    Specs::Inode readInode(int32_t idx);
    void writeInode(int32_t idx, const Specs::Inode& node);

private:
    static const size_t CACHE_CAPACITY;

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

#endif // DISK_CONTROLLER_HPP