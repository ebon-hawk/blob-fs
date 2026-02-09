#include <cstring>
#include <stdexcept>

#include "disk_controller.hpp"

const size_t DiskController::CACHE_CAPACITY = 500;

DiskController::DiskController(std::fstream& fs, const Specs::Superblock& sb)
    : fs(fs), sb(sb), inodeCache(CACHE_CAPACITY, this) {
    loadBitmaps();
}

DiskController::~DiskController() {
    try {
        // Final flush of bitmaps and dirty inodes
        sync();
    }
    catch (...) {
        // Destructors should not throw exceptions
    }
}

// --- INITIALIZATION ---
void DiskController::loadBitmaps() {
    inodeBitmap.resize(Specs::bitsToBytes(sb.inodeCount));

    fs.seekg(sb.inodeBitmapOffset, std::ios::beg);

    if (!fs.read(reinterpret_cast<char*>(inodeBitmap.data()), inodeBitmap.size())) {
        throw std::runtime_error("Failed to read inode bitmap.");
    }

    blockBitmap.resize(Specs::bitsToBytes(sb.dataBlockCount));
    fs.seekg(sb.blockBitmapOffset, std::ios::beg);

    if (!fs.read(reinterpret_cast<char*>(blockBitmap.data()), blockBitmap.size())) {
        throw std::runtime_error("Failed to read block bitmap.");
    }
}

// --- INODE MANAGEMENT ---
InodeEntry* DiskController::getInode(int32_t idx) {
    InodeEntry* cached = inodeCache.get(idx);

    if (cached) return cached;

    // Cache miss
    Specs::Inode node = readInode(idx);

    return inodeCache.put(idx, { std::move(node), false });
}

int32_t DiskController::copyInode(int32_t srcIdx, int32_t parentIdx) {
    InodeEntry* srcEntry = getInode(srcIdx);

    if (!srcEntry) return Specs::NULL_INDEX;

    Specs::Inode srcNode = srcEntry->node;

    int32_t newIdx = allocateInode();

    if (newIdx == Specs::NULL_INDEX) return Specs::NULL_INDEX;

    Specs::Inode newNode = srcNode;
    newNode.parent = parentIdx;

    // Initialize list pointers for the new copy
    // to prevent pointing to source's relatives
    newNode.firstChild = Specs::NULL_INDEX;
    newNode.nextSibling = Specs::NULL_INDEX;
    newNode.prevSibling = Specs::NULL_INDEX;

    // Reset block pointers so we don't point to the original's blocks
    for (int i = 0; i < 32; ++i) newNode.directBlocks[i] = Specs::NULL_INDEX;

    newNode.indirectBlock = Specs::NULL_INDEX;

    if (newNode.isDirectory) {
        updateInode(newIdx, newNode);

        return newIdx;
    }

    try {
        // Copy direct blocks
        for (int i = 0; i < 32; ++i) {
            if (srcNode.directBlocks[i] == Specs::NULL_INDEX) continue;

            int32_t blockIdx = allocateBlock();

            if (blockIdx == Specs::NULL_INDEX) {
                throw std::runtime_error("Disk is full.");
            }

            std::vector<char> buffer(sb.blockSize);

            readBlock(srcNode.directBlocks[i], buffer.data());
            writeBlock(blockIdx, buffer.data());

            newNode.directBlocks[i] = blockIdx;
        }

        // Copy indirect block
        if (srcNode.indirectBlock != Specs::NULL_INDEX) {
            uint32_t ptrs = sb.blockSize / sizeof(int32_t);

            int32_t newIndIdx = allocateBlock();

            if (newIndIdx == Specs::NULL_INDEX) {
                throw std::runtime_error("Disk is full.");
            }

            std::vector<int32_t> srcTable(ptrs);

            std::vector<int32_t> newTable(ptrs, Specs::NULL_INDEX);

            readBlock(srcNode.indirectBlock, reinterpret_cast<char*>(srcTable.data()));

            for (uint32_t i = 0; i < ptrs; ++i) {
                if (srcTable[i] == Specs::NULL_INDEX) continue;

                int32_t dataBlock = allocateBlock();

                if (dataBlock == Specs::NULL_INDEX) {
                    throw std::runtime_error("Disk is full.");
                }

                std::vector<char> buffer(sb.blockSize);

                readBlock(srcTable[i], buffer.data());
                writeBlock(dataBlock, buffer.data());

                newTable[i] = dataBlock;
            }

            writeBlock(newIndIdx, reinterpret_cast<char*>(newTable.data()));

            newNode.indirectBlock = newIndIdx;
        }
    }
    catch (...) {
        updateInode(newIdx, newNode);

        freeInode(newIdx);

        return Specs::NULL_INDEX;
    }

    updateInode(newIdx, newNode);

    return newIdx;
}

void DiskController::freeInode(int32_t idx) {
    InodeEntry* entry = getInode(idx);

    if (!entry) return;

    Specs::Inode& node = entry->node;

    if (!node.isDirectory) {
        // Mark direct blocks as available in the RAM vector
        for (int i = 0; i < 32; ++i) {
            if (node.directBlocks[i] != Specs::NULL_INDEX) {
                freeBlock(node.directBlocks[i]);
                node.directBlocks[i] = Specs::NULL_INDEX;
            }
        }

        // Handle indirect blocks
        if (node.indirectBlock != Specs::NULL_INDEX) {
            uint32_t pPerB = sb.blockSize / sizeof(int32_t);

            std::vector<int32_t> indirectBuffer(pPerB);

            readBlock(node.indirectBlock, reinterpret_cast<char*>(indirectBuffer.data()));

            for (uint32_t i = 0; i < pPerB; ++i) {
                if (indirectBuffer[i] != Specs::NULL_INDEX) {
                    freeBlock(indirectBuffer[i]);
                }
            }

            // Mark the block that held the pointers as available
            freeBlock(node.indirectBlock);
            node.indirectBlock = Specs::NULL_INDEX;
        }
    }

    // Mark the inode itself as available in the RAM vector
    inodeBitmap[idx / 8] &= ~(1 << (idx % 8));

    // We do NOT sync here; we let the driver decide when to commit to disk
    inodeBitmapDirty = true;

    if (idx < (int32_t)lastInodeHint) {
        lastInodeHint = idx;
    }

    entry->dirty = false;

    // Remove from cache entirely
    inodeCache.remove(idx);
}

void DiskController::updateInode(int32_t idx, const Specs::Inode& node) {
    // This handles both adding new entries and updating existing ones
    inodeCache.put(idx, { node, true });
}

// --- BITMAP OPERATIONS ---
int32_t DiskController::allocateBlock() {
    int32_t idx = findFreeBit(blockBitmap, sb.dataBlockCount, lastBlockHint);

    if (idx != Specs::NULL_INDEX) {
        blockBitmap[idx / 8] |= (1 << (idx % 8));
        blockBitmapDirty = true;
    }

    return idx;
}

int32_t DiskController::allocateInode() {
    int32_t idx = findFreeBit(inodeBitmap, sb.inodeCount, lastInodeHint);

    if (idx != Specs::NULL_INDEX) {
        inodeBitmap[idx / 8] |= (1 << (idx % 8));
        inodeBitmapDirty = true;
    }

    return idx;
}

void DiskController::sync() {
    if (inodeBitmapDirty) {
        fs.seekp(sb.inodeBitmapOffset, std::ios::beg);
        fs.write(reinterpret_cast<const char*>(inodeBitmap.data()), inodeBitmap.size());
        inodeBitmapDirty = false;
    }

    if (blockBitmapDirty) {
        fs.seekp(sb.blockBitmapOffset, std::ios::beg);
        fs.write(reinterpret_cast<const char*>(blockBitmap.data()), blockBitmap.size());

        blockBitmapDirty = false;
    }

    using CacheIt =
        std::unordered_map<int32_t, LRUCache<int32_t, InodeEntry>::CacheEntry>::iterator;

    for (CacheIt it = inodeCache.begin(); it != inodeCache.end(); ++it) {
        if (it->second.value.dirty) {
            writeInode(it->first, it->second.value.node);

            it->second.value.dirty = false;
        }
    }

    fs.flush();
}

// --- I/O ---
void DiskController::readBlock(int32_t blockIdx, char* buffer) {
    if (blockIdx < 0 || blockIdx >= sb.dataBlockCount) {
        throw std::out_of_range("Invalid block index.");
    }

    uint32_t offset = sb.dataRegionOffset + (blockIdx * sb.blockSize);

    fs.seekg(offset, std::ios::beg);

    fs.read(buffer, sb.blockSize);
}

void DiskController::writeBlock(int32_t blockIdx, const char* buffer) {
    if (blockIdx < 0 || blockIdx >= sb.dataBlockCount) {
        throw std::out_of_range("Invalid block index.");
    }

    uint32_t offset = sb.dataRegionOffset + (blockIdx * sb.blockSize);

    fs.seekp(offset, std::ios::beg);
    fs.write(buffer, sb.blockSize);
}

// --- INTERFACE IMPLEMENTATION ---
bool DiskController::onEvict(const int32_t& key, InodeEntry& value) {
    if (!value.dirty) return true;

    try {
        writeInode(key, value.node);

        value.dirty = false;

        return true;
    }
    catch (...) {
        return false;
    }
}

int32_t DiskController::findFreeBit(const std::vector<uint8_t>& bitmap,
    uint32_t totalCount, uint32_t& hint) {
    uint32_t startByte = (hint / 8);
    uint32_t totalBytes = bitmap.size();

    // Search from hint to end of bitmap
    for (uint32_t i = startByte; i < totalBytes; ++i) {
        // Only check individual bits if the byte isn't full
        if (bitmap[i] != 0xFF) {
            for (int bit = 0; bit < 8; ++bit) {
                int32_t currentIdx = (i * 8) + bit;

                // Safety check
                if (currentIdx >= (int32_t)totalCount) break;

                // Skip bits before the exact hint within the 'startByte'
                if (currentIdx < (int32_t)hint) continue;

                if (!(bitmap[i] & (1 << bit))) {
                    // Update hint for next time
                    hint = currentIdx;

                    return currentIdx;
                }
            }
        }
    }

    // Wrap-around (search from beginning to hint)
    for (uint32_t i = 0; i <= startByte; ++i) {
        if (bitmap[i] != 0xFF) {
            for (int bit = 0; bit < 8; ++bit) {
                int32_t currentIdx = (i * 8) + bit;

                // We stop once we reach the original hint
                if (currentIdx >= (int32_t)hint || currentIdx >= (int32_t)totalCount) break;

                if (!(bitmap[i] & (1 << bit))) {
                    hint = currentIdx;

                    return currentIdx;
                }
            }
        }
    }

    return Specs::NULL_INDEX;
}

void DiskController::freeBlock(int32_t blockIdx) {
    if (blockIdx == Specs::NULL_INDEX) return;

    uint32_t byteIdx = blockIdx / 8;

    // Locate the bit in our RAM-resident vector
    uint32_t bitIdx = blockIdx % 8;

    // Flip the bit to 0
    blockBitmap[byteIdx] &= ~(1 << bitIdx);
    blockBitmapDirty = true;

    // Update hint so we reuse this low-index block sooner
    if (blockIdx < (int32_t)lastBlockHint) {
        lastBlockHint = blockIdx;
    }
}

Specs::Inode DiskController::readInode(int32_t idx) {
    if (idx < 0 || idx >= sb.inodeCount) {
        throw std::out_of_range("Invalid inode index.");
    }

    Specs::Inode node;
    uint32_t offset = sb.inodeTableOffset + (idx * Specs::INODE_SIZE);

    fs.seekg(offset, std::ios::beg);

    fs.read(reinterpret_cast<char*>(&node), Specs::INODE_SIZE);

    return node;
}

void DiskController::writeInode(int32_t idx, const Specs::Inode& node) {
    uint32_t offset = sb.inodeTableOffset + (idx * Specs::INODE_SIZE);

    fs.seekp(offset, std::ios::beg);
    fs.write(reinterpret_cast<const char*>(&node), Specs::INODE_SIZE);
}