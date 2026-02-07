#include <stdexcept>

#include "disk_controller.hpp"

const size_t DiskController::CACHE_CAPACITY = 100;

void DiskController::loadBitmaps(std::fstream& fs, const Specs::Superblock& sb) {
    inodeBitmap.resize(Specs::bitsToBytes(sb.inodeCount));

    fs.seekg(sb.inodeBitmapOffset, std::ios::beg);

    if (!fs.read(reinterpret_cast<char*>(inodeBitmap.data()), inodeBitmap.size())) {
        throw std::runtime_error("Failed to read inode bitmap from disk.");
    }

    dataBitmap.resize(Specs::bitsToBytes(sb.dataBlockCount));
    fs.seekg(sb.blockBitmapOffset, std::ios::beg);

    if (!fs.read(reinterpret_cast<char*>(dataBitmap.data()), dataBitmap.size())) {
        throw std::runtime_error("Failed to read data bitmap from disk.");
    }
}

Specs::Inode DiskController::getInode(std::fstream& fs,
    const Specs::Superblock& sb, int32_t idx) {
    std::unordered_map<int32_t, CacheEntry>::iterator it = inodeCache.find(idx);

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
    enforceCacheLimit(fs, sb);

    // Add new entry to the front
    lruList.push_front(idx);

    CacheEntry newNode;
    newNode.dirty = false;
    newNode.it = lruList.begin();
    newNode.node = node;

    inodeCache[idx] = newNode;

    return node;
}

void DiskController::freeInode(std::fstream& fs,
    const Specs::Superblock& sb, int32_t idx) {
    // Fetch the inode to find its block pointers
    Specs::Inode node = getInode(fs, sb, idx);

    if (!node.isDirectory) {
        // Mark direct blocks as available in the RAM vector
        for (int32_t i = 0; i < 32; ++i) {
            freeBlock(node.directBlocks[i]);
            node.directBlocks[i] = Specs::NULL_INDEX;
        }

        // Handle indirect blocks
        if (node.indirectBlock != Specs::NULL_INDEX) {
            uint32_t pointersPerBlock = sb.blockSize / sizeof(int32_t);

            std::vector<int32_t> indirectBuffer(pointersPerBlock);

            readBlock(fs, sb, node.indirectBlock, reinterpret_cast<char*>(indirectBuffer.data()));

            for (uint32_t i = 0; i < (int32_t)pointersPerBlock; ++i) {
                freeBlock(indirectBuffer[i]);
            }

            // Mark the block that held the pointers as available
            freeBlock(node.indirectBlock);
            node.indirectBlock = Specs::NULL_INDEX;
        }
    }

    node.isDirectory = 0;
    node.size = 0;

    // Update the disk/cache with the now "clean" inode
    updateInode(fs, sb, idx, node);

    // Mark the inode itself as available in the RAM vector
    inodeBitmap[idx / 8] &= ~(1 << (idx % 8));

    // We do NOT sync here; we let the driver decide when to commit to disk
    inodeBitmapDirty = true;

    // LRU eviction
    std::unordered_map<int32_t, CacheEntry>::iterator it = inodeCache.find(idx);

    if (inodeCache.end() != it) {
        lruList.erase(it->second.it);

        inodeCache.erase(it);
    }
}

void DiskController::updateInode(std::fstream& fs, const Specs::Superblock& sb,
    int32_t idx, const Specs::Inode& node) {
    // Synchronize the cache
    std::unordered_map<int32_t, CacheEntry>::iterator it = inodeCache.find(idx);

    // If it exists in cache, update the data and move it to the front
    if (inodeCache.end() != it) {
        it->second.node = node;
        lruList.erase(it->second.it);
        lruList.push_front(idx);

        it->second.dirty = true;
        it->second.it = lruList.begin();
    }
    // If it doesn't exist in cache, just add it as the most recently used
    else {
        enforceCacheLimit(fs, sb);
        lruList.push_front(idx);

        CacheEntry newNode;
        newNode.dirty = false;
        newNode.it = lruList.begin();
        newNode.node = node;

        inodeCache[idx] = newNode;
    }
}

int32_t DiskController::allocateBlock(std::fstream& fs, const Specs::Superblock& sb) {
    int32_t idx = findFreeBit(dataBitmap, sb.dataBlockCount);

    if (idx != Specs::NULL_INDEX) {
        dataBitmap[idx / 8] |= (1 << (idx % 8));
        dataBitmapDirty = true;
    }

    return idx;
}

int32_t DiskController::allocateInode(std::fstream& fs, const Specs::Superblock& sb) {
    int32_t idx = findFreeBit(inodeBitmap, sb.inodeCount);

    if (idx != Specs::NULL_INDEX) {
        inodeBitmap[idx / 8] |= (1 << (idx % 8));
        inodeBitmapDirty = true;
    }

    return idx;
}

void DiskController::sync(std::fstream& fs, const Specs::Superblock& sb) {
    if (inodeBitmapDirty) {
        fs.seekp(sb.inodeBitmapOffset, std::ios::beg);
        fs.write(reinterpret_cast<const char*>(inodeBitmap.data()), inodeBitmap.size());
        inodeBitmapDirty = false;
    }

    if (dataBitmapDirty) {
        fs.seekp(sb.blockBitmapOffset, std::ios::beg);
        fs.write(reinterpret_cast<const char*>(dataBitmap.data()), dataBitmap.size());

        dataBitmapDirty = false;
    }

    for (std::pair<const int32_t, CacheEntry>& pair : inodeCache) {
        if (pair.second.dirty) {
            writeInode(fs, sb, pair.first, pair.second.node);

            pair.second.dirty = false;
        }
    }

    // Ensure the OS actually pushes the data to the physical drive
    fs.flush();
}

void DiskController::readBlock(std::fstream& fs, const Specs::Superblock& sb,
    int32_t blockIdx, char* buffer) {
    if (blockIdx == Specs::NULL_INDEX) return;

    uint32_t offset = sb.dataRegionOffset + (blockIdx * sb.blockSize);

    fs.seekg(offset, std::ios::beg);

    fs.read(buffer, sb.blockSize);
}

void DiskController::writeBlock(std::fstream& fs, const Specs::Superblock& sb,
    int32_t blockIdx, const char* buffer) {
    if (blockIdx == Specs::NULL_INDEX) return;

    uint32_t offset = sb.dataRegionOffset + (blockIdx * sb.blockSize);

    fs.seekp(offset, std::ios::beg);
    fs.write(buffer, sb.blockSize);
}

int32_t DiskController::findFreeBit(const std::vector<uint8_t>& bitmap, uint32_t totalCount) {
    const uint64_t* data64 = reinterpret_cast<const uint64_t*>(bitmap.data());
    size_t numWords = bitmap.size() / 8;

    for (size_t i = 0; i < numWords; ++i) {
        // 0xFFFFFFFFFFFFFFFF means all 64 bits are 1 (full)
        if (data64[i] != ~0ULL) {
            // There is at least one 0 here
            for (int bit = 0; bit < 64; ++bit) {
                uint64_t mask = 1ULL << bit;

                if (!(data64[i] & mask)) {
                    int32_t foundIdx = (i * 64) + bit;

                    return (foundIdx < (int32_t)totalCount) ? foundIdx : Specs::NULL_INDEX;
                }
            }
        }
    }

    // Handle remaining bytes if the bitmap size isn't a multiple of 8
    for (size_t i = numWords * 8; i < bitmap.size(); ++i) {
        if (bitmap[i] != 0xFF) {
            for (int bit = 0; bit < 8; ++bit) {
                if (!(bitmap[i] & (1 << bit))) {
                    int32_t foundIdx = (i * 8) + bit;

                    return (foundIdx < (int32_t)totalCount) ? foundIdx : Specs::NULL_INDEX;
                }
            }
        }
    }

    return Specs::NULL_INDEX;
}

void DiskController::enforceCacheLimit(std::fstream& fs, const Specs::Superblock& sb) {
    if (inodeCache.size() >= CACHE_CAPACITY) {
        int32_t lastIdx = lruList.back();

        std::unordered_map<int32_t, CacheEntry>::iterator it = inodeCache.find(lastIdx);

        if (it->second.dirty) {
            writeInode(fs, sb, lastIdx, it->second.node);
        }

        lruList.pop_back();

        inodeCache.erase(lastIdx);
    }
}

void DiskController::freeBlock(int32_t blockIdx) {
    if (blockIdx == Specs::NULL_INDEX) return;

    uint32_t byteIdx = blockIdx / 8;

    uint32_t bitIdx = blockIdx % 8;

    // Flip the bit in the RAM-resident vector
    dataBitmap[byteIdx] &= ~(1 << bitIdx);
    dataBitmapDirty = true;
}

Specs::Inode DiskController::readInode(std::fstream& fs,
    const Specs::Superblock& sb, int32_t idx) {
    Specs::Inode node;
    uint32_t offset = sb.inodeTableOffset + (idx * Specs::INODE_SIZE);

    fs.seekg(offset, std::ios::beg);

    fs.read(reinterpret_cast<char*>(&node), Specs::INODE_SIZE);

    return node;
}

void DiskController::writeInode(std::fstream& fs, const Specs::Superblock& sb,
    int32_t idx, const Specs::Inode& node) {
    uint32_t offset = sb.inodeTableOffset + (idx * Specs::INODE_SIZE);

    fs.seekp(offset, std::ios::beg);
    fs.write(reinterpret_cast<const char*>(&node), Specs::INODE_SIZE);
}