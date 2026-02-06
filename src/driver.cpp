#include "driver.hpp"

int32_t Driver::resolvePath(std::fstream& fs, const Specs::Superblock& sb,
    int32_t currentDirIdx, const std::string& path) {
    if (path.empty()) return currentDirIdx;

    // Root is always 0
    if (path == "/") return 0;

    std::vector<std::string> tokens = tokenize(path);

    // Start from root if path starts with '/', otherwise start from currentDir
    int32_t searchIdx = (path[0] == '/') ? 0 : currentDirIdx;

    if (tokens.empty()) return searchIdx;

    for (const std::string& targetName : tokens) {
        Specs::Inode currentInode = getInode(fs, sb, searchIdx);

        // ERROR: Path components must be directories
        if (!currentInode.isDirectory) {
            return Specs::NULL_INDEX;
        }

        if (targetName == ".") continue;

        if (targetName == "..") {
            searchIdx = currentInode.parent;

            // Cap at root
            if (searchIdx == Specs::NULL_INDEX) searchIdx = 0;

            continue;
        }

        // Search through children
        bool found = false;
        int32_t childIdx = currentInode.firstChild;

        while (childIdx != Specs::NULL_INDEX) {
            Specs::Inode child = getInode(fs, sb, childIdx);

            if (targetName == child.name) {
                searchIdx = childIdx;
                found = true;
                break;
            }

            childIdx = child.nextSibling;
        }

        // Path broken
        if (!found) return Specs::NULL_INDEX;
    }

    return searchIdx;
}

std::vector<std::string> Driver::tokenize(const std::string& path) {
    std::vector<std::string> tokens;
    std::stringstream ss(path);
    std::string item;

    while (std::getline(ss, item, '/')) {
        if (!item.empty()) tokens.push_back(item);
    }

    return tokens;
}

bool Driver::matchesPattern(const char* pattern, const char* name) {
    // Both exhausted (match found)
    if (*pattern == '\0' && *name == '\0') return true;

    if (*pattern == '*') {
        // Matches empty string (skip '*') or
        // current char (skip name char, keep '*')
        return matchesPattern(pattern + 1, name) ||
            (*name != '\0' && matchesPattern(pattern, name + 1));
    }

    if (*pattern == '?' && *name != '\0') {
        return matchesPattern(pattern + 1, name + 1);
    }

    // Standard char match
    if (*pattern == *name) {
        return matchesPattern(pattern + 1, name + 1);
    }

    return false;
}

std::vector<int32_t> Driver::findMatchesInDirectory(std::fstream& fs,
    const Specs::Superblock& sb, int32_t dirIdx, const std::string& pattern) {
    std::vector<int32_t> matches;

    Specs::Inode dir = getInode(fs, sb, dirIdx);

    // Only directories have children to match
    if (!dir.isDirectory) return matches;

    int32_t currentIdx = dir.firstChild;

    while (currentIdx != Specs::NULL_INDEX) {
        Specs::Inode entry = getInode(fs, sb, currentIdx);

        if (matchesPattern(pattern.c_str(), entry.name)) {
            matches.push_back(currentIdx);
        }

        currentIdx = entry.nextSibling;
    }

    return matches;
}

void Driver::addEntryToDirectory(std::fstream& fs, const Specs::Superblock& sb,
    int32_t parentIdx, int32_t childIdx) {
    Specs::Inode parent = getInode(fs, sb, parentIdx);

    Specs::Inode child = getInode(fs, sb, childIdx);
    child.parent = parentIdx;

    // Parent is currently empty
    if (parent.firstChild == Specs::NULL_INDEX) {
        parent.firstChild = childIdx;
        child.prevSibling = Specs::NULL_INDEX;
        child.nextSibling = Specs::NULL_INDEX;
    }
    // Parent already has children (push new child to the front)
    else {
        int32_t oldHeadIdx = parent.firstChild;

        Specs::Inode oldHead = getInode(fs, sb, oldHeadIdx);
        child.nextSibling = oldHeadIdx;
        child.prevSibling = Specs::NULL_INDEX;
        oldHead.prevSibling = childIdx;
        parent.firstChild = childIdx;

        // Save the old head because its prevSibling pointer changed
        updateInode(fs, sb, oldHeadIdx, oldHead);
    }

    // Save the child and the parent
    updateInode(fs, sb, childIdx, child);
    updateInode(fs, sb, parentIdx, parent);
}

void Driver::moveEntry(std::fstream& fs, const Specs::Superblock& sb,
    int32_t currentDirIdx, const std::string& srcPath, const std::string& destPath) {
    int32_t fileIdx = resolvePath(fs, sb, currentDirIdx, srcPath);

    int32_t destDirIdx = resolvePath(fs, sb, currentDirIdx, destPath);

    if (fileIdx == Specs::NULL_INDEX) {
        std::cerr << "mv: Invalid source path.\n";

        return;
    }

    if (destDirIdx == Specs::NULL_INDEX) {
        std::cerr << "mv: Invalid destination path.\n";

        return;
    }

    unlinkInode(fs, sb, fileIdx);

    addEntryToDirectory(fs, sb, destDirIdx, fileIdx);

    std::cout << "Moved successfully.\n";
}

void Driver::unlinkInode(std::fstream& fs, const Specs::Superblock& sb, int32_t targetIdx) {
    Specs::Inode target = getInode(fs, sb, targetIdx);
    int32_t pIdx = target.parent;

    Specs::Inode parent = getInode(fs, sb, pIdx);

    // If I am the head child, move the parent's pointer to my next sibling
    if (parent.firstChild == targetIdx) {
        parent.firstChild = target.nextSibling;
    }

    // If someone is after me, tell them to point back to my predecessor
    if (target.nextSibling != Specs::NULL_INDEX) {
        Specs::Inode nextSib = getInode(fs, sb, target.nextSibling);
        nextSib.prevSibling = target.prevSibling;
        updateInode(fs, sb, target.nextSibling, nextSib);
    }

    // If someone is before me, tell them to point forward to my successor
    if (target.prevSibling != Specs::NULL_INDEX) {
        Specs::Inode prevSib = getInode(fs, sb, target.prevSibling);
        prevSib.nextSibling = target.nextSibling;
        updateInode(fs, sb, target.prevSibling, prevSib);
    }

    // Finalize the parent's new state
    updateInode(fs, sb, pIdx, parent);
}

// TODO: Remove hard-coded magic numbers
int32_t Driver::getPhysicalBlockIdx(std::fstream& fs, const Specs::Superblock& sb,
    const Specs::Inode& node, uint32_t logicalBlockIdx) {
    // Direct block
    if (logicalBlockIdx < 32) {
        return node.directBlocks[logicalBlockIdx];
    }

    // Indirect block
    if (node.indirectBlock == Specs::NULL_INDEX) return Specs::NULL_INDEX;

    // We need to read the table of pointers from the disk
    std::vector<int32_t> indirectTable(sb.blockSize / sizeof(int32_t));

    readBlock(fs, sb, node.indirectBlock, reinterpret_cast<char*>(indirectTable.data()));
    uint32_t tableIdx = logicalBlockIdx - 32;

    if (tableIdx < indirectTable.size()) {
        return indirectTable[tableIdx];
    }

    return Specs::NULL_INDEX;
}

void Driver::attachBlockToInode(std::fstream& fs, Specs::Superblock& sb,
    Specs::Inode& node, int32_t blockIdx) {
    // Nudge the numerator so that anything more than a perfect multiple
    // of the block size gets pushed over the edge to the next integer
    uint32_t currentBlockCount = (node.size + sb.blockSize - 1) / sb.blockSize;

    if (!node.size) currentBlockCount = 0;

    if (currentBlockCount < 32) {
        node.directBlocks[currentBlockCount] = blockIdx;
    }
    else {
        // We are in the indirect zone
        if (node.indirectBlock == Specs::NULL_INDEX) {
            // Allocate the "table" block itself first
            node.indirectBlock = findFreeBlock(fs, sb);
            std::vector<int32_t> emptyTable(sb.blockSize / sizeof(int32_t), Specs::NULL_INDEX);
            writeBlock(fs, sb, node.indirectBlock, reinterpret_cast<char*>(emptyTable.data()));
        }

        // Read the table, update the specific slot, write it back
        std::vector<int32_t> table(sb.blockSize / sizeof(int32_t));

        readBlock(fs, sb, node.indirectBlock, reinterpret_cast<char*>(table.data()));
        table[currentBlockCount - 32] = blockIdx;
        writeBlock(fs, sb, node.indirectBlock, reinterpret_cast<char*>(table.data()));
    }
}