#include <stdexcept>

#include "disk_controller.hpp"

const size_t DiskController::INODE_CACHE_CAPACITY = 250;

DiskController::DiskController(std::fstream& fs, const Specs::Superblock& sb)
    : fs(fs), sb(sb), inodeCache(INODE_CACHE_CAPACITY, this) {
    loadBitmaps();
}

DiskController::~DiskController() {
    try {
        sync();
    }
    catch (...) {}
}

// --- PERSISTENCE CONTROL ---
void DiskController::sync() {
    if (inodeBitmapDirty) {
        fs.seekp(sb.inodeBitmapOffset, std::ios::beg);
        fs.write(reinterpret_cast<const char*>(inodeBitmap.data()), inodeBitmap.size());

        if (!fs) {
            throw std::runtime_error("Failed to sync inode bitmap.");
        }

        inodeBitmapDirty = false;
    }

    if (blockBitmapDirty) {
        fs.seekp(sb.blockBitmapOffset, std::ios::beg);
        fs.write(reinterpret_cast<const char*>(blockBitmap.data()), blockBitmap.size());

        if (!fs) {
            throw std::runtime_error("Failed to sync block bitmap.");
        }

        blockBitmapDirty = false;
    }

    inodeCache.forEach([this](int32_t key, InodeEntry& entry) {
        if (entry.dirty) {
            writeInodeToDisk(key, entry.node);

            entry.dirty = false;
        }
        });

    fs.flush();

    if (!fs) {
        throw std::runtime_error("Failed to flush file system.");
    }
}

// --- RESOURCE ALLOCATION ---
int32_t DiskController::allocateBlock() {
    return allocateResource(blockBitmap, sb.blockCount, lastBlockHint, blockBitmapDirty);
}

int32_t DiskController::allocateInode() {
    return allocateResource(inodeBitmap, sb.inodeCount, lastInodeHint, inodeBitmapDirty);
}

int32_t DiskController::createInode(int32_t parentIdx, const std::string& name, bool isDirectory) {
    int32_t newIdx = allocateInode();

    if (newIdx == Specs::NULL_INDEX) {
        return Specs::NULL_INDEX;
    }

    Specs::Inode node{};
    std::strncpy(node.name, name.c_str(), sizeof(node.name) - 1);

    node.name[sizeof(node.name) - 1] = '\0';

    node.isDirectory = isDirectory;
    node.parent = parentIdx;
    node.size = 0;

    node.firstChild = node.nextSibling = node.prevSibling = Specs::NULL_INDEX;
    node.indirectBlock = Specs::NULL_INDEX;

    for (uint32_t i = 0; i < Specs::DIRECT_BLOCKS_COUNT; ++i) {
        node.directBlocks[i] = Specs::NULL_INDEX;
    }

    updateInode(newIdx, node);

    return newIdx;
}

void DiskController::freeInode(int32_t inodeIdx) {
    if (inodeIdx == Specs::NULL_INDEX) {
        return;
    }

    InodeEntry* entry = getInode(inodeIdx);

    if (!entry) {
        return;
    }

    freeData(entry->node);
    toggleBit(inodeBitmap, inodeIdx, false);

    inodeBitmapDirty = true;
    inodeCache.remove(inodeIdx);
}

// --- INODE ACCESS & MANIPULATION ---
InodeEntry* DiskController::getInode(int32_t inodeIdx) {
    InodeEntry* cached = inodeCache.get(inodeIdx);

    if (cached) {
        return cached;
    }

    Specs::Inode node = readInodeFromDisk(inodeIdx);

    return inodeCache.put(inodeIdx, { std::move(node), false });
}

int32_t DiskController::copyInode(int32_t sourceIdx, int32_t parentIdx, const std::string& newName) {
    InodeEntry* src = getInode(sourceIdx);

    if (!src) {
        return Specs::NULL_INDEX;
    }

    int32_t newIdx = allocateInode();

    if (newIdx == Specs::NULL_INDEX) {
        return Specs::NULL_INDEX;
    }

    Specs::Inode newNode = src->node;
    newNode.firstChild = newNode.nextSibling = newNode.prevSibling = Specs::NULL_INDEX;
    newNode.indirectBlock = Specs::NULL_INDEX;
    newNode.parent = parentIdx;

    for (uint32_t i = 0; i < Specs::DIRECT_BLOCKS_COUNT; ++i) {
        newNode.directBlocks[i] = Specs::NULL_INDEX;
    }

    if (!newName.empty()) {
        std::strncpy(newNode.name, newName.c_str(), sizeof(newNode.name) - 1);

        newNode.name[sizeof(newNode.name) - 1] = '\0';
    }

    try {
        if (!newNode.isDirectory) {
            copyData(src->node, newNode);
        }

        updateInode(newIdx, newNode);

        return newIdx;
    }
    catch (...) {
        freeInode(newIdx);

        return Specs::NULL_INDEX;
    }
}

void DiskController::updateInode(int32_t inodeIdx, const Specs::Inode& node) {
    inodeCache.put(inodeIdx, { node, true });
}

// --- BLOCK I/O ---
void DiskController::readBlock(int32_t blockIdx, char* buffer) {
    if (blockIdx < 0 || blockIdx >= (int32_t)sb.blockCount) {
        throw std::out_of_range("Invalid block index.");
    }

    uint32_t offset = getBlockOffset(blockIdx);

    fs.seekg(offset, std::ios::beg);

    fs.read(buffer, sb.blockSize);
}

void DiskController::writeBlock(int32_t blockIdx, const char* buffer) {
    if (blockIdx < 0 || blockIdx >= (int32_t)sb.blockCount) {
        throw std::out_of_range("Invalid block index.");
    }

    uint32_t offset = getBlockOffset(blockIdx);

    fs.seekp(offset, std::ios::beg);
    fs.write(buffer, sb.blockSize);
}

// --- CACHE EVENT HANDLER IMPLEMENTATION ---
bool DiskController::onEvict(const int32_t& key, InodeEntry& value) {
    if (!value.dirty) {
        return true;
    }

    try {
        writeInodeToDisk(key, value.node);

        value.dirty = false;

        return true;
    }
    catch (...) {
        return false;
    }
}

// --- FILE GROWTH ---
bool DiskController::canGrowFile(const Specs::Inode& node, uint32_t bytesToAdd) {
    if (bytesToAdd == 0) {
        return true;
    }

    // Check inode max size
    if (static_cast<uint64_t>(node.size) + bytesToAdd > maxFileSizeBytes()) {
        return false;
    }

    // Calculate current blocks and needed blocks
    uint32_t currentBlocks = (node.size + sb.blockSize - 1) / sb.blockSize;
    uint32_t totalBlocksAfterGrowth = (node.size + bytesToAdd + sb.blockSize - 1) / sb.blockSize;

    uint32_t newBlocksRequired = (totalBlocksAfterGrowth > currentBlocks)
        ? (totalBlocksAfterGrowth - currentBlocks)
        : 0;

    // Check global disk availability
    return newBlocksRequired <= getFreeDiskBlocks();
}

void DiskController::extendInode(Specs::Inode& node, uint32_t newTotalSize) {
    if (newTotalSize <= node.size) {
        return;
    }

    uint32_t currentBlocks = (node.size + sb.blockSize - 1) / sb.blockSize;
    uint32_t targetBlocks = (newTotalSize + sb.blockSize - 1) / sb.blockSize;

    std::vector<int32_t> allocatedBlocks;

    try {
        for (uint32_t i = currentBlocks; i < targetBlocks; ++i) {
            int32_t bIdx = allocateBlock();

            if (bIdx == Specs::NULL_INDEX) {
                throw std::runtime_error("Disk full.");
            }

            allocatedBlocks.push_back(bIdx);

            if (i < Specs::DIRECT_BLOCKS_COUNT) {
                node.directBlocks[i] = bIdx;
            }
            else {
                uint32_t indirectPos = i - Specs::DIRECT_BLOCKS_COUNT;

                linkToIndirect(node, indirectPos, bIdx);
            }
        }

        node.size = newTotalSize;
    }
    catch (...) {
        // Rollback partially allocated blocks
        for (int32_t bIdx : allocatedBlocks) {
            freeBlock(bIdx);
        }

        throw;
    }
}

void DiskController::linkToIndirect(Specs::Inode& node, uint32_t pos, int32_t blockIdx) {
    uint32_t ptrsPerBlock = sb.blockSize / sizeof(int32_t);

    if (pos >= ptrsPerBlock) {
        throw std::out_of_range("Indirect block position out of range.");
    }

    std::vector<int32_t> table(ptrsPerBlock, Specs::NULL_INDEX);

    if (node.indirectBlock == Specs::NULL_INDEX) {
        node.indirectBlock = allocateBlock();

        if (node.indirectBlock == Specs::NULL_INDEX) {
            throw std::runtime_error("Disk full.");
        }
    }
    else {
        readBlock(node.indirectBlock, reinterpret_cast<char*>(table.data()));
    }

    table[pos] = blockIdx;
    writeBlock(node.indirectBlock, reinterpret_cast<char*>(table.data()));
}

uint32_t DiskController::getFreeDiskBlocks() const {
    uint32_t count = 0;

    for (size_t byteIdx = 0; byteIdx < blockBitmap.size(); ++byteIdx) {
        uint8_t byte = blockBitmap[byteIdx];

        if (byte == 0) {
            count += 8;
        }
        else if (byte == 0xFF) {
            continue;
        }
        else {
            for (int bit = 0; bit < 8; ++bit) {
                int32_t blockIdx = static_cast<int32_t>(byteIdx * 8 + bit);

                if (blockIdx >= (int32_t)sb.blockCount) {
                    break;
                }

                if (!(byte & (1 << bit))) {
                    ++count;
                }
            }
        }
    }

    return count;
}

uint32_t DiskController::maxBlocksPerInode() const {
    // Number of pointers an indirect block can hold
    uint32_t ptrsPerBlock = sb.blockSize / sizeof(int32_t);

    return Specs::DIRECT_BLOCKS_COUNT + ptrsPerBlock;
}

uint64_t DiskController::maxFileSizeBytes() const {
    return static_cast<uint64_t>(maxBlocksPerInode()) * sb.blockSize;
}

// --- DEEP COPY ---
void DiskController::copyData(const Specs::Inode& src, Specs::Inode& dest) {
    std::vector<int32_t> allocatedBlocks;

    std::vector<char> buffer(sb.blockSize);

    try {
        for (uint32_t i = 0; i < Specs::DIRECT_BLOCKS_COUNT; ++i) {
            if (src.directBlocks[i] == Specs::NULL_INDEX) {
                continue;
            }

            int32_t newBlock = allocateBlock();

            if (newBlock == Specs::NULL_INDEX) {
                throw std::runtime_error("Disk full.");
            }

            allocatedBlocks.push_back(newBlock);
            readBlock(src.directBlocks[i], buffer.data());
            writeBlock(newBlock, buffer.data());

            dest.directBlocks[i] = newBlock;
        }

        if (src.indirectBlock != Specs::NULL_INDEX) {
            copyIndirectData(src.indirectBlock, dest.indirectBlock);
        }
    }
    catch (...) {
        for (int32_t bIdx : allocatedBlocks) {
            freeBlock(bIdx);
        }

        if (dest.indirectBlock != Specs::NULL_INDEX) {
            freeIndirectData(dest.indirectBlock);

            dest.indirectBlock = Specs::NULL_INDEX;
        }

        throw;
    }
}

void DiskController::copyIndirectData(int32_t srcIdx, int32_t& destIdx) {
    if (srcIdx == Specs::NULL_INDEX) {
        return;
    }

    const uint32_t ptrsPerBlock = sb.blockSize / sizeof(int32_t);
    std::vector<char> buffer(sb.blockSize);
    std::vector<int32_t> destTable(ptrsPerBlock, Specs::NULL_INDEX);
    std::vector<int32_t> srcTable(ptrsPerBlock);

    readBlock(srcIdx, reinterpret_cast<char*>(srcTable.data()));

    destIdx = allocateBlock();

    if (destIdx == Specs::NULL_INDEX) {
        throw std::runtime_error("Disk full.");
    }

    try {
        for (uint32_t i = 0; i < ptrsPerBlock; ++i) {
            if (srcTable[i] == Specs::NULL_INDEX) {
                continue;
            }

            int32_t newBlock = allocateBlock();

            if (newBlock == Specs::NULL_INDEX) {
                throw std::runtime_error("Disk full.");
            }

            readBlock(srcTable[i], buffer.data());
            writeBlock(newBlock, buffer.data());

            destTable[i] = newBlock;
        }

        writeBlock(destIdx, reinterpret_cast<char*>(destTable.data()));
    }
    catch (...) {
        for (int32_t b : destTable) {
            if (b != Specs::NULL_INDEX) {
                freeBlock(b);
            }
        }

        freeBlock(destIdx);

        destIdx = Specs::NULL_INDEX;

        throw;
    }
}

void DiskController::freeData(Specs::Inode& node) {
    // Free direct blocks
    for (uint32_t i = 0; i < Specs::DIRECT_BLOCKS_COUNT; ++i) {
        if (node.directBlocks[i] != Specs::NULL_INDEX) {
            freeBlock(node.directBlocks[i]);
            node.directBlocks[i] = Specs::NULL_INDEX;
        }
    }

    // Free indirect block and its data
    if (node.indirectBlock != Specs::NULL_INDEX) {
        freeIndirectData(node.indirectBlock);
        node.indirectBlock = Specs::NULL_INDEX;
    }

    node.size = 0;
}

void DiskController::freeIndirectData(int32_t idx) {
    if (idx == Specs::NULL_INDEX) {
        return;
    }

    const uint32_t ptrsPerBlock = sb.blockSize / sizeof(int32_t);
    std::vector<int32_t> table(ptrsPerBlock);

    readBlock(idx, reinterpret_cast<char*>(table.data()));

    for (uint32_t i = 0; i < ptrsPerBlock; ++i) {
        if (table[i] != Specs::NULL_INDEX) {
            freeBlock(table[i]);
        }
    }

    freeBlock(idx);
}

// --- DISK I/O ---
uint32_t DiskController::getBlockOffset(int32_t blockIdx) const {
    return sb.dataRegionOffset + (blockIdx * sb.blockSize);
}

uint32_t DiskController::getInodeOffset(int32_t inodeIdx) const {
    return sb.inodeTableOffset + (inodeIdx * sizeof(Specs::Inode));
}

Specs::Inode DiskController::readInodeFromDisk(int32_t inodeIdx) {
    if (inodeIdx < 0 || inodeIdx >= (int32_t)sb.inodeCount) {
        throw std::out_of_range("Invalid inode index.");
    }

    Specs::Inode node;
    uint32_t offset = getInodeOffset(inodeIdx);

    fs.seekg(offset, std::ios::beg);

    fs.read(reinterpret_cast<char*>(&node), sizeof(Specs::Inode));

    if (!fs) {
        throw std::runtime_error("Disk I/O failure.");
    }

    return node;
}

void DiskController::writeInodeToDisk(int32_t inodeIdx, const Specs::Inode& node) {
    if (inodeIdx < 0 || inodeIdx >= (int32_t)sb.inodeCount) {
        throw std::out_of_range("Invalid inode index.");
    }

    uint32_t offset = getInodeOffset(inodeIdx);

    fs.seekp(offset, std::ios::beg);
    fs.write(reinterpret_cast<const char*>(&node), sizeof(Specs::Inode));

    if (!fs) {
        throw std::runtime_error("Disk I/O failure.");
    }
}

// --- BITMAP MANAGEMENT ---
int32_t DiskController::allocateResource(std::vector<uint8_t>& bitmap, uint32_t totalCount, uint32_t& hint, bool& dirtyFlag) {
    int32_t idx = findFreeBit(bitmap, totalCount, hint);

    if (idx != Specs::NULL_INDEX) {
        toggleBit(bitmap, idx, true);

        dirtyFlag = true;
    }

    return idx;
}

int32_t DiskController::findFreeBit(const std::vector<uint8_t>& bitmap, uint32_t totalCount, uint32_t& hint) {
    uint32_t startByte = (hint / 8);
    uint32_t totalBytes = bitmap.size();

    // Search from hint to end of bitmap
    for (uint32_t i = startByte; i < totalBytes; ++i) {
        // Only check individual bits if the byte isn't full
        if (bitmap[i] != 0xFF) {
            for (int bit = 0; bit < 8; ++bit) {
                int32_t currentIdx = (i * 8) + bit;

                // Safety check
                if (currentIdx >= (int32_t)totalCount) {
                    break;
                }

                // Skip bits before the exact hint within the 'startByte'
                if (currentIdx < (int32_t)hint) {
                    continue;
                }

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
                if (currentIdx >= (int32_t)hint || currentIdx >= (int32_t)totalCount) {
                    break;
                }

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
    if (blockIdx == Specs::NULL_INDEX) {
        return;
    }

    if (blockIdx < 0 || blockIdx >= (int32_t)sb.blockCount) {
        throw std::out_of_range("Block index out of range.");
    }

    toggleBit(blockBitmap, blockIdx, false);

    blockBitmapDirty = true;

    if (blockIdx < (int32_t)lastBlockHint) {
        lastBlockHint = (uint32_t)blockIdx;
    }
}

void DiskController::loadBitmaps() {
    inodeBitmap.resize(Specs::bitsToBytes(sb.inodeCount));

    fs.seekg(sb.inodeBitmapOffset, std::ios::beg);

    if (!fs.read(reinterpret_cast<char*>(inodeBitmap.data()), inodeBitmap.size())) {
        throw std::runtime_error("Failed to load inode bitmap.");
    }

    blockBitmap.resize(Specs::bitsToBytes(sb.blockCount));
    fs.seekg(sb.blockBitmapOffset, std::ios::beg);

    if (!fs.read(reinterpret_cast<char*>(blockBitmap.data()), blockBitmap.size())) {
        throw std::runtime_error("Failed to load block bitmap.");
    }
}

void DiskController::toggleBit(std::vector<uint8_t>& bitmap, int32_t idx, bool set) {
    if (idx < 0 || idx >= (int32_t)(bitmap.size() * 8)) {
        throw std::out_of_range("Bitmap index out of range.");
    }

    if (set) {
        bitmap[idx / 8] |= (1 << (idx % 8));
    }
    else {
        bitmap[idx / 8] &= ~(1 << (idx % 8));
    }
}