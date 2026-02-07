#include <iostream>
#include <sstream>

#include "file_system.hpp"

int32_t FileSystem::resolvePath(const std::string& path) {
    if (path.empty()) return currentDirIdx;

    // Root is always 0
    if (path == "/") return 0;

    std::vector<std::string> tokens = tokenize(path);

    // Start from root if path starts with '/', otherwise start from 'currentDir'
    int32_t searchIdx = (path[0] == '/') ? 0 : currentDirIdx;

    if (tokens.empty()) return searchIdx;

    for (const std::string& targetName : tokens) {
        if (targetName == ".") {
            // Stay in current directory
            continue;
        }

        Specs::Inode currentInode = controller.getInode(fs, sb, searchIdx);

        // Path components must be directories
        if (!currentInode.isDirectory) {
            return Specs::NULL_INDEX;
        }

        if (targetName == "..") {
            searchIdx = currentInode.parent;

            // Cap at root
            if (searchIdx == Specs::NULL_INDEX) searchIdx = 0;

            continue;
        }

        // Search children by name
        bool found = false;
        int32_t childIdx = currentInode.firstChild;

        while (childIdx != Specs::NULL_INDEX) {
            const Specs::Inode& child = controller.getInode(fs, sb, childIdx);

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

void FileSystem::sync() {
    controller.sync(fs, sb);
    fs.flush();
}

int32_t FileSystem::findChildInDirectory(int32_t dirIdx, const std::string& name) {
    Specs::Inode dirNode = controller.getInode(fs, sb, dirIdx);
    int32_t currentChildIdx = dirNode.firstChild;

    while (currentChildIdx != Specs::NULL_INDEX) {
        Specs::Inode child = controller.getInode(fs, sb, currentChildIdx);

        if (child.name == name) {
            return currentChildIdx;
        }

        currentChildIdx = child.nextSibling;
    }

    return Specs::NULL_INDEX;
}

std::vector<int32_t> FileSystem::findAllMatches(int32_t dirIdx, const std::string& pattern) {
    std::vector<int32_t> matches;

    Specs::Inode dirNode = controller.getInode(fs, sb, dirIdx);
    int32_t currentChildIdx = dirNode.firstChild;

    while (currentChildIdx != Specs::NULL_INDEX) {
        Specs::Inode child = controller.getInode(fs, sb, currentChildIdx);

        if (matchesPattern(pattern, child.name)) {
            matches.push_back(currentChildIdx);
        }

        currentChildIdx = child.nextSibling;
    }

    return matches;
}

std::vector<std::string> FileSystem::tokenize(const std::string& path, char delimiter) {
    std::vector<std::string> tokens;

    std::stringstream ss(path);

    std::string token;

    while (std::getline(ss, token, delimiter)) {
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }

    return tokens;
}

void FileSystem::addEntryToDirectory(int32_t parentIdx, int32_t childIdx) {
    Specs::Inode parent = controller.getInode(fs, sb, parentIdx);

    Specs::Inode child = controller.getInode(fs, sb, childIdx);
    child.parent = parentIdx;

    // Parent is currently empty
    if (parent.firstChild == Specs::NULL_INDEX) {
        parent.firstChild = childIdx;

        child.nextSibling = Specs::NULL_INDEX;
        child.prevSibling = Specs::NULL_INDEX;
    }
    // Parent already has children (push new child to the front)
    else {
        int32_t oldHeadIdx = parent.firstChild;

        Specs::Inode oldHead = controller.getInode(fs, sb, oldHeadIdx);
        child.nextSibling = oldHeadIdx;
        child.prevSibling = Specs::NULL_INDEX;
        oldHead.prevSibling = childIdx;
        parent.firstChild = childIdx;

        // Save the old head because its 'prevSibling' pointer changed
        controller.updateInode(fs, sb, oldHeadIdx, oldHead);
    }

    controller.updateInode(fs, sb, childIdx, child);
    controller.updateInode(fs, sb, parentIdx, parent);
}

void FileSystem::deleteInodeRecursive(int32_t idx) {
    if (idx == Specs::NULL_INDEX) return;

    Specs::Inode node = controller.getInode(fs, sb, idx);

    // If it's a directory, delete all children first
    int32_t currentChild = node.firstChild;

    while (currentChild != Specs::NULL_INDEX) {
        // We need to fetch the next sibling before deleting the child
        Specs::Inode childNode = controller.getInode(fs, sb, currentChild);
        int32_t next = childNode.nextSibling;

        deleteInodeRecursive(currentChild);

        currentChild = next;
    }

    // This handles both direct and indirect blocks internally
    controller.freeInode(fs, sb, idx);
}

void FileSystem::unlinkInodeFromParent(int32_t targetIdx) {
    Specs::Inode target = controller.getInode(fs, sb, targetIdx);
    int32_t pIdx = target.parent;

    if (pIdx == Specs::NULL_INDEX) return;

    Specs::Inode parent = controller.getInode(fs, sb, pIdx);

    // If I am the head child, move the parent's pointer to my next sibling
    if (parent.firstChild == targetIdx) {
        parent.firstChild = target.nextSibling;
    }

    // If someone is after me, tell them to point back to my predecessor
    if (target.nextSibling != Specs::NULL_INDEX) {
        Specs::Inode nextSib = controller.getInode(fs, sb, target.nextSibling);
        nextSib.prevSibling = target.prevSibling;

        controller.updateInode(fs, sb, target.nextSibling, nextSib);
    }

    // If someone is before me, tell them to point forward to my successor
    if (target.prevSibling != Specs::NULL_INDEX) {
        Specs::Inode prevSib = controller.getInode(fs, sb, target.prevSibling);
        prevSib.nextSibling = target.nextSibling;

        controller.updateInode(fs, sb, target.prevSibling, prevSib);
    }

    target.parent = Specs::NULL_INDEX;

    target.nextSibling = Specs::NULL_INDEX;
    target.prevSibling = Specs::NULL_INDEX;

    // Finalize the parent's new state
    controller.updateInode(fs, sb, pIdx, parent);
}

bool FileSystem::matchesPattern(const std::string& pattern, const std::string& name) {
    size_t n = name.length();
    size_t p = pattern.length();

    size_t i = 0, j = 0, match = 0, startIndex = std::string::npos;

    while (i < n) {
        // If characters match or pattern has '?'
        if (j < p && (pattern[j] == '?' || pattern[j] == name[i])) {
            ++i;
            ++j;
        }
        // If pattern has '*', mark the position and try to match 0 characters
        else if (j < p && pattern[j] == '*') {
            startIndex = j;

            match = i;

            ++j;
        }
        // If last match was a '*', backtrack and try matching one more character
        else if (startIndex != std::string::npos) {
            j = startIndex + 1;
            match++;

            i = match;
        }
        else {
            return false;
        }
    }

    // Handle trailing '*' in pattern (e.g., "file**")
    while (j < p && pattern[j] == '*') {
        ++j;
    }

    return j == p;
}

void FileSystem::attachBlockToInode(Specs::Inode& node,
    int32_t inodeIdx, int32_t blockIdx) {
    // Nudge the numerator so that anything more than a perfect multiple
    // of the block size gets pushed over the edge to the next integer
    uint32_t currentBlockCount = (node.size == 0) ?
        0 : (node.size + sb.blockSize - 1) / sb.blockSize;

    if (currentBlockCount < 32) {
        node.directBlocks[currentBlockCount] = blockIdx;
    }
    else {
        // We are in the indirect zone
        if (node.indirectBlock == Specs::NULL_INDEX) {
            // Allocate the "table" block itself first
            node.indirectBlock = controller.allocateBlock(fs, sb);
            std::vector<int32_t> emptyTable(sb.blockSize / sizeof(int32_t), Specs::NULL_INDEX);

            controller.writeBlock(fs, sb, node.indirectBlock, reinterpret_cast<char*>(emptyTable.data()));
        }

        std::vector<int32_t> table(sb.blockSize / sizeof(int32_t));

        controller.readBlock(fs, sb, node.indirectBlock, reinterpret_cast<char*>(table.data()));
        table[currentBlockCount - 32] = blockIdx;

        controller.writeBlock(fs, sb, node.indirectBlock, reinterpret_cast<char*>(table.data()));
    }

    // Finalize the inode state in the cache
    controller.updateInode(fs, sb, inodeIdx, node);
}

bool FileSystem::isDirectoryEmpty(int32_t dirIdx) {
    Specs::Inode node = controller.getInode(fs, sb, dirIdx);

    return node.firstChild == Specs::NULL_INDEX;
}

bool FileSystem::isValidFilename(const std::string& name) {
    if (name.empty() || name.length() >= 64) return false;

    // Check for reserved characters
    const std::string illegalChars = "/\\:*?\"<>|";

    if (name.find_first_of(illegalChars) != std::string::npos) return false;

    // Prevent "." and ".." as manual names
    if (name == "." || name == "..") return false;

    return true;
}