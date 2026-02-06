#include <unordered_map>

#include "disk_controller.hpp"

namespace {
    constexpr size_t CACHE_CAPACITY = 100;

    struct CacheNode {
        Specs::Inode node;
        std::list<int32_t>::iterator it;
    };

    std::unordered_map<int32_t, CacheNode> inodeCache;

    // Stores inode indices, most recent at the front
    std::list<int32_t> lruList;
}

Specs::Inode getInode(std::fstream& fs, const Specs::Superblock& sb, int32_t idx) {
    std::unordered_map<int32_t, CacheNode>::iterator it = inodeCache.find(idx);

    // Cache hit
    if (inodeCache.end() != it) {
        // Use the stored iterator to erase the element from its old position in O(1)
        lruList.erase(it->second.it);

        // Move to front (most recently used)
        lruList.push_front(idx);

        // Update the stored iterator to the new front position
        it->second.it = lruList.begin();

        return it->second.node;
    }

    // Cache miss
    Specs::Inode node = readInode(fs, sb, idx);

    // If cache is full, evict the least recently used (back of the list)
    if (CACHE_CAPACITY <= inodeCache.size()) {
        int32_t lastIdx = lruList.back();
        lruList.pop_back();

        inodeCache.erase(lastIdx);
    }

    // Add new entry to the front
    lruList.push_front(idx);

    CacheNode newNode;
    newNode.it = lruList.begin();
    newNode.node = node;

    inodeCache[idx] = newNode;

    return node;
}

void updateInode(std::fstream& fs, const Specs::Superblock& sb, int32_t idx, const Specs::Inode& node) {
    // Always persist to disk first
    writeInode(fs, sb, idx, node);

    // Synchronize the cache
    std::unordered_map<int32_t, CacheNode>::iterator it = inodeCache.find(idx);

    if (inodeCache.end() != it) {
        // If it exists in cache, update the data and move it to the front
        it->second.node = node;
        lruList.erase(it->second.it);
        lruList.push_front(idx);

        it->second.it = lruList.begin();
    }
    else {
        // If it doesn't exist in cache, just add it as the most recently used
        if (CACHE_CAPACITY <= inodeCache.size()) {
            int32_t lastIdx = lruList.back();
            lruList.pop_back();

            inodeCache.erase(lastIdx);
        }

        lruList.push_front(idx);

        CacheNode newNode;
        newNode.it = lruList.begin();
        newNode.node = node;

        inodeCache[idx] = newNode;
    }
}

int32_t findFreeInode(std::fstream& fs, const Specs::Superblock& sb) {
    uint32_t bitmapSize = (sb.inodeCount / 8) + 1;

    std::vector<uint8_t> bitmap(bitmapSize);

    // Read the inode bitmap into RAM
    fs.seekg(sb.inodeBitmapOffset, std::ios::beg);

    fs.read(reinterpret_cast<char*>(bitmap.data()), bitmapSize);

    for (uint32_t i = 0; i < sb.inodeCount; ++i) {
        uint32_t byteIdx = i / 8;

        uint32_t bitIdx = i % 8;

        // Check if free
        if (!(bitmap[byteIdx] & (1 << bitIdx))) {
            // Mark as used
            bitmap[byteIdx] |= (1 << bitIdx);

            // Write bitmap back to disk immediately
            fs.seekp(sb.inodeBitmapOffset, std::ios::beg);
            fs.write(reinterpret_cast<char*>(bitmap.data()), bitmapSize);

            return (int32_t)i;
        }
    }

    return -1;
}

int32_t findFreeBlock(std::fstream& fs, Specs::Superblock& sb) {
    uint32_t numBlocks = sb.maxFileSize / sb.blockSize;
    uint32_t bitmapSize = (numBlocks / 8) + 1;
    std::vector<uint8_t> bitmap(bitmapSize);
    fs.seekg(sb.blockBitmapOffset, std::ios::beg);
    fs.read(reinterpret_cast<char*>(bitmap.data()), bitmapSize);

    for (uint32_t i = 0; i < numBlocks; ++i) {
        uint32_t byteIdx = i / 8;

        uint32_t bitIdx = i % 8;

        if (!(bitmap[byteIdx] & (1 << bitIdx))) {
            // Mark as used
            bitmap[byteIdx] |= (1 << bitIdx);

            // Write bitmap back to disk immediately
            fs.seekp(sb.blockBitmapOffset, std::ios::beg);
            fs.write(reinterpret_cast<char*>(bitmap.data()), bitmapSize);

            return (int32_t)i;
        }
    }

    return -1;
}

Specs::Inode readInode(std::fstream& fs, const Specs::Superblock& sb, int32_t idx) {
    Specs::Inode node;
    uint32_t offset = sb.inodeTableOffset + (idx * sizeof(Specs::Inode));

    fs.seekg(offset, std::ios::beg);

    fs.read(reinterpret_cast<char*>(&node), sizeof(Specs::Inode));

    return node;
}

void writeInode(std::fstream& fs, const Specs::Superblock& sb, int32_t idx, const Specs::Inode& node) {
    uint32_t offset = sb.inodeTableOffset + (idx * sizeof(Specs::Inode));

    fs.seekp(offset, std::ios::beg);
    fs.write(reinterpret_cast<const char*>(&node), sizeof(Specs::Inode));

    fs.flush();
}

void readBlock(std::fstream& fs, const Specs::Superblock& sb, int32_t blockIdx, char* buffer) {
    uint32_t offset = sb.dataRegionOffset + (blockIdx * sb.blockSize);

    fs.seekg(offset, std::ios::beg);

    fs.read(buffer, sb.blockSize);
}

void writeBlock(std::fstream& fs, const Specs::Superblock& sb, int32_t blockIdx, const char* buffer) {
    uint32_t offset = sb.dataRegionOffset + (blockIdx * sb.blockSize);

    fs.seekp(offset, std::ios::beg);
    fs.write(buffer, sb.blockSize);

    fs.flush();
}

Specs::Superblock initNewDisk(std::fstream& fs, uint64_t maxSize) {
    Specs::Superblock sb;
    sb.blockSize = Specs::BLOCK_SIZE;
    sb.magicNumber = Specs::MAGIC_NUMBER;

    // Estimate counts (20% for inodes, 80% for data)
    sb.dataBlockCount = (0.8 * maxSize) / sb.blockSize;
    sb.inodeCount = (0.2 * maxSize) / sizeof(Specs::Inode);

    // Calculate offsets
    sb.inodeBitmapOffset = sizeof(Specs::Superblock);

    sb.blockBitmapOffset = sb.inodeBitmapOffset + (sb.inodeCount / 8) + 1;
    sb.inodeTableOffset = sb.blockBitmapOffset + (sb.dataBlockCount / 8) + 1;

    // Align data region to block size
    sb.dataRegionOffset = ((sb.inodeTableOffset + (sb.inodeCount * sizeof(Specs::Inode)) + Specs::BLOCK_SIZE - 1) / Specs::BLOCK_SIZE) * Specs::BLOCK_SIZE;

    // Write zeros to the file to "reserve" the space on the real disk
    std::vector<char> empty(Specs::BLOCK_SIZE, 0);

    for (uint64_t i = 0; i < maxSize; i += Specs::BLOCK_SIZE) {
        fs.write(empty.data(), std::min((uint64_t)Specs::BLOCK_SIZE, maxSize - i));
    }

    // Create root inode at index 0
    Specs::Inode root;
    std::memset(&root, 0, sizeof(root));
    std::strcpy(root.name, "/");

    root.isDirectory = 1;
    root.parent = Specs::NULL_INDEX;

    root.firstChild = Specs::NULL_INDEX;
    root.nextSibling = Specs::NULL_INDEX;
    root.prevSibling = Specs::NULL_INDEX;

    // Write superblock and root
    fs.seekp(0, std::ios::beg);
    fs.write(reinterpret_cast<char*>(&sb), sizeof(sb));

    // We can't use updateInode because the disk isn't "live" yet,
    // so we call our raw write function
    writeInode(fs, sb, 0, root);

    // Manually mark inode 0 as used in the bitmap
    uint8_t firstByte = 0x01;

    fs.seekp(sb.inodeBitmapOffset, std::ios::beg);
    fs.write(reinterpret_cast<char*>(&firstByte), 1);

    return sb;
}