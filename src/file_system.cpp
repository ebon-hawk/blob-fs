#include <iostream>
#include <stdexcept>

#include "file_system.hpp"
#include "string_utils.hpp"

// --- DATA HELPERS ---
Dentry* FileSystem::getOrPopulateDentry(int32_t dirIdx) {
    Dentry* cached = dirCache.get(dirIdx);

    if (cached) return cached;

    InodeEntry* entry = controller.getInode(dirIdx);

    if (!entry || !entry->node.isDirectory) {
        return nullptr;
    }

    Dentry newDentry;
    int32_t childIdx = entry->node.firstChild;

    // Iterate through the linked list of siblings on disk
    while (childIdx != Specs::NULL_INDEX) {
        InodeEntry* childEntry = controller.getInode(childIdx);

        if (childEntry) {
            // Map the filename string to its inode index
            newDentry.nameToInode[childEntry->node.name] = childIdx;

            childIdx = childEntry->node.nextSibling;
        }
        else {
            break;
        }
    }

    return dirCache.put(dirIdx, newDentry);
}

int32_t FileSystem::findChildInDirectory(int32_t dirIdx, const std::string& name) {
    Dentry* dentry = getOrPopulateDentry(dirIdx);

    if (!dentry) {
        return Specs::NULL_INDEX;
    }

    std::unordered_map<std::string, int32_t>::iterator it = dentry->nameToInode.find(name);

    if (it != dentry->nameToInode.end()) {
        return it->second;
    }

    return Specs::NULL_INDEX;
}

std::string FileSystem::getFullPath(int32_t idx) {
    if (!idx) return "/";

    if (idx == Specs::NULL_INDEX) return "";

    int32_t curr = idx;
    std::string path = "";

    while (curr != 0 && curr != Specs::NULL_INDEX) {
        InodeEntry* entry = controller.getInode(curr);

        if (!entry) break;

        // Build path backwards
        path = "/" + std::string(entry->node.name) + path;

        // Move to parent
        curr = entry->node.parent;
    }

    return path.empty() ? "/" : path;
}

std::vector<int32_t> FileSystem::findAllMatches(int32_t dirIdx, const std::string& pattern) {
    std::vector<int32_t> matches;

    // Ensure directory is loaded into the RAM cache
    Dentry* dentry = getOrPopulateDentry(dirIdx);

    if (!dentry) {
        return matches;
    }

    // Iterate through the cached name-to-inode map
    std::unordered_map<std::string, int32_t>::iterator it;

    for (it = dentry->nameToInode.begin(); it != dentry->nameToInode.end(); ++it) {
        const std::string& fileName = it->first;
        int32_t inodeIdx = it->second;

        if (StringUtils::matchesPattern(pattern, fileName)) {
            matches.push_back(inodeIdx);
        }
    }

    return matches;
}

void FileSystem::attachBlockToInode(Specs::Inode& node, int32_t inodeIdx, int32_t blockIdx) {
    // Calculate the logical block index for the new block
    uint32_t currentBlockCount = (node.size == 0) ?
        0 : (node.size + sb.blockSize - 1) / sb.blockSize;

    if (currentBlockCount < 32) {
        node.directBlocks[currentBlockCount] = blockIdx;
    }
    else {
        uint32_t ptrsPerBlock = sb.blockSize / sizeof(int32_t);

        uint32_t indirectTableIdx = currentBlockCount - 32;

        if (indirectTableIdx >= ptrsPerBlock) {
            throw std::runtime_error("File exceeds maximum size (indirect block full).");
        }

        if (node.indirectBlock == Specs::NULL_INDEX) {
            int32_t tableBlockIdx = controller.allocateBlock();

            if (tableBlockIdx == Specs::NULL_INDEX) {
                throw std::runtime_error("Disk full: failed to allocate indirect block.");
            }

            node.indirectBlock = tableBlockIdx;

            // Zero-out the new table block immediately
            std::vector<int32_t> emptyTable(ptrsPerBlock, Specs::NULL_INDEX);

            controller.writeBlock(node.indirectBlock, reinterpret_cast<const char*>(emptyTable.data()));
        }

        std::vector<int32_t> table(ptrsPerBlock);

        controller.readBlock(node.indirectBlock, reinterpret_cast<char*>(table.data()));
        table[indirectTableIdx] = blockIdx;

        controller.writeBlock(node.indirectBlock, reinterpret_cast<const char*>(table.data()));
    }

    controller.updateInode(inodeIdx, node);
}

void FileSystem::updateCurrentDir(int32_t newIdx) {
    if (newIdx == Specs::NULL_INDEX) return;

    InodeEntry* entry = controller.getInode(newIdx);

    if (!entry || !entry->node.isDirectory) {
        throw std::runtime_error("Path is not a directory.");
    }

    if (currentDirIdx != newIdx) {
        prevDirIdx = currentDirIdx;

        currentDirIdx = newIdx;
    }
}

// --- TREE MANIPULATION HELPERS ---
void FileSystem::addEntryToDirectory(int32_t parentIdx, int32_t childIdx) {
    InodeEntry* parentEntry = controller.getInode(parentIdx);

    InodeEntry* childEntry = controller.getInode(childIdx);

    if (!childEntry || !parentEntry) return;

    Specs::Inode& childNode = childEntry->node;
    Specs::Inode& parentNode = parentEntry->node;

    // Set the basic parent linkage
    childNode.parent = parentIdx;

    // Update the doubly-linked list of siblings
    if (parentNode.firstChild == Specs::NULL_INDEX) {
        // Parent is currently empty
        parentNode.firstChild = childIdx;

        childNode.nextSibling = Specs::NULL_INDEX;
        childNode.prevSibling = Specs::NULL_INDEX;
    }
    else {
        // Parent already has children (push new child to the front)
        int32_t oldHeadIdx = parentNode.firstChild;

        InodeEntry* oldHeadEntry = controller.getInode(oldHeadIdx);

        if (oldHeadEntry) {
            childNode.nextSibling = oldHeadIdx;
            childNode.prevSibling = Specs::NULL_INDEX;
            oldHeadEntry->node.prevSibling = childIdx;

            controller.updateInode(oldHeadIdx, oldHeadEntry->node);
        }

        parentNode.firstChild = childIdx;
    }

    // If this directory's name-map is in RAM, we must add the new entry immediately
    Dentry* cachedDir = dirCache.get(parentIdx);

    if (cachedDir) {
        cachedDir->nameToInode[childNode.name] = childIdx;
    }

    controller.updateInode(childIdx, childNode);
    controller.updateInode(parentIdx, parentNode);
}

void FileSystem::deleteInodeRecursive(int32_t idx) {
    if (!idx || idx == Specs::NULL_INDEX) {
        // Never recursively delete the root
        return;
    }

    InodeEntry* entry = controller.getInode(idx);

    if (!entry) return;

    // If it's a directory, clean out the children first
    if (entry->node.isDirectory) {
        int32_t currentChildIdx = entry->node.firstChild;

        while (currentChildIdx != Specs::NULL_INDEX) {
            // We must fetch the sibling pointer BEFORE deleting the child
            InodeEntry* childEntry = controller.getInode(currentChildIdx);

            if (!childEntry) break;

            int32_t nextSiblingIdx = childEntry->node.nextSibling;

            // Recurse down
            deleteInodeRecursive(currentChildIdx);

            currentChildIdx = nextSiblingIdx;
        }

        // Since the directory is being deleted, its name-map is now garbage
        dirCache.remove(idx);
    }

    // This handles both direct and indirect blocks internally
    controller.freeInode(idx);
}

void FileSystem::unlinkInodeFromParent(int32_t targetIdx) {
    InodeEntry* targetEntry = controller.getInode(targetIdx);

    if (!targetEntry) return;

    Specs::Inode& targetNode = targetEntry->node;
    int32_t pIdx = targetNode.parent;

    if (pIdx == Specs::NULL_INDEX) return;

    InodeEntry* parentEntry = controller.getInode(pIdx);

    if (!parentEntry) return;

    Specs::Inode& parentNode = parentEntry->node;

    // If I am the head child, move the parent's pointer to my next sibling
    if (parentNode.firstChild == targetIdx) {
        parentNode.firstChild = targetNode.nextSibling;
    }

    // If someone is after me, tell them to point back to my predecessor
    if (targetNode.nextSibling != Specs::NULL_INDEX) {
        InodeEntry* nextSibEntry = controller.getInode(targetNode.nextSibling);

        if (nextSibEntry) {
            nextSibEntry->node.prevSibling = targetNode.prevSibling;

            controller.updateInode(targetNode.nextSibling, nextSibEntry->node);
        }
    }

    // If someone is before me, tell them to point forward to my successor
    if (targetNode.prevSibling != Specs::NULL_INDEX) {
        InodeEntry* prevSibEntry = controller.getInode(targetNode.prevSibling);

        if (prevSibEntry) {
            prevSibEntry->node.nextSibling = targetNode.nextSibling;

            controller.updateInode(targetNode.prevSibling, prevSibEntry->node);
        }
    }

    Dentry* cachedDir = dirCache.get(pIdx);

    if (cachedDir) {
        cachedDir->nameToInode.erase(targetNode.name);
    }

    // Reset target linkage
    targetNode.parent = Specs::NULL_INDEX;

    targetNode.nextSibling = Specs::NULL_INDEX;
    targetNode.prevSibling = Specs::NULL_INDEX;

    controller.updateInode(pIdx, parentNode);
    controller.updateInode(targetIdx, targetNode);
}

// --- EDGE CASE VALIDATORS ---
bool FileSystem::isDirectoryEmpty(int32_t dirIdx) {
    InodeEntry* entry = controller.getInode(dirIdx);

    if (!entry || !entry->node.isDirectory) {
        return false;
    }

    return entry->node.firstChild == Specs::NULL_INDEX;
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