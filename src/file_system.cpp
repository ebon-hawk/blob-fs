#include <iostream>
#include <stdexcept>

#include "file_system.hpp"
#include "string_utils.hpp"

// --- DIRECTORY OPERATIONS ---
void FileSystem::cd(const std::string& path) {
    if (path.empty()) {
        std::cout << getFullPath(currentDirIdx) << std::endl;

        return;
    }

    if (path == "~") {
        updateCurrentDir(0);

        return;
    }

    // Toggle to previous directory
    if (path == "-") {
        if (prevDirIdx == Specs::NULL_INDEX) {
            throw std::runtime_error("[cd] OLDPWD not set.");
        }

        std::cout << getFullPath(prevDirIdx) << std::endl;
        updateCurrentDir(prevDirIdx);

        return;
    }

    // Resolve the provided path (handles absolute, relative, '.', and '..')
    int32_t targetIdx = resolvePath(path);

    if (targetIdx == Specs::NULL_INDEX) {
        throw std::runtime_error("[cd] No such file or directory.");
    }

    try {
        updateCurrentDir(targetIdx);
    }
    catch (const std::runtime_error& e) {
        throw std::runtime_error("[cd] " + std::string(e.what()));
    }
}

void FileSystem::ls(const std::string& path) {
    int32_t targetDirIdx = Specs::NULL_INDEX;

    // Default to "show everything"
    std::string pattern = "*";

    if (path.empty()) {
        targetDirIdx = currentDirIdx;
    }
    else {
        // Check if the input contains wildcards
        if (path.find_first_of("*?") != std::string::npos) {
            size_t lastSlash = path.find_last_of('/');

            if (lastSlash == std::string::npos) {
                // For example, "ls *.txt" (current directory, pattern is "*.txt")
                targetDirIdx = currentDirIdx;

                pattern = path;
            }
            else {
                // For example, "ls /home/user/*.cpp"
                std::string dirPart = path.substr(0, lastSlash);

                // Handle leading slash root case
                if (dirPart.empty()) dirPart = "/";

                pattern = path.substr(lastSlash + 1);
                targetDirIdx = resolvePath(dirPart);
            }
        }
        else {
            targetDirIdx = resolvePath(path);

            // If the user points to a specific file, just show that file
            InodeEntry* entry = controller.getInode(targetDirIdx);

            if (entry && !entry->node.isDirectory) {
                std::cout << "f\t" << entry->node.size << "\t" << entry->node.name << "\n";

                return;
            }
        }
    }

    if (targetDirIdx == Specs::NULL_INDEX) {
        throw std::runtime_error("[ls] No such file or directory.");
    }

    InodeEntry* dirEntry = controller.getInode(targetDirIdx);

    if (!dirEntry || !dirEntry->node.isDirectory) {
        throw std::runtime_error("[ls] Not a directory.");
    }

    std::vector<int32_t> matches = findAllMatches(targetDirIdx, pattern);

    if (matches.empty()) return;

    std::cout << "Type\tSize\tName\n";

    std::cout << "-------------------------------------------\n";

    for (int32_t inodeIdx : matches) {
        InodeEntry* entry = controller.getInode(inodeIdx);

        if (!entry) continue;

        char type = entry->node.isDirectory ? 'd' : 'f';
        std::cout << type << "\t" << entry->node.size << "\t" << entry->node.name << "\n";
    }

    std::cout << std::endl;
}

void FileSystem::mkdir(const std::string& path) {
    if (path.empty()) {
        throw std::runtime_error("[mkdir] Missing operand.");
    }

    std::vector<std::string> tokens = StringUtils::tokenize(path);

    if (tokens.empty()) {
        throw std::runtime_error("[mkdir] Invalid path.");
    }

    std::string newDirName = tokens.back();
    tokens.pop_back();

    if (!isValidFilename(newDirName)) {
        throw std::runtime_error("[mkdir] Invalid directory name.");
    }

    std::string parentPath = (path[0] == '/') ? "/" : "";

    for (size_t i = 0; i < tokens.size(); ++i) {
        parentPath += tokens[i] + (i == tokens.size() - 1 ? "" : "/");
    }

    int32_t parentIdx = resolvePath(parentPath);

    if (parentIdx == Specs::NULL_INDEX) {
        throw std::runtime_error("[mkdir] Parent directory does not exist.");
    }

    if (findChildInDirectory(parentIdx, newDirName) != Specs::NULL_INDEX) {
        throw std::runtime_error("[mkdir] File or directory already exists.");
    }

    int32_t newDirIdx = controller.allocateInode();

    if (newDirIdx == Specs::NULL_INDEX) {
        throw std::runtime_error("[mkdir] Disk full.");
    }

    InodeEntry* entry = controller.getInode(newDirIdx);
    Specs::Inode& node = entry->node;
    node.isDirectory = true;
    node.size = 0;

    node.firstChild = Specs::NULL_INDEX;
    node.parent = parentIdx;

    node.nextSibling = Specs::NULL_INDEX;
    node.prevSibling = Specs::NULL_INDEX;
    std::strncpy(node.name, newDirName.c_str(), sizeof(node.name) - 1);

    node.name[sizeof(node.name) - 1] = '\0';

    for (int i = 0; i < 32; ++i) node.directBlocks[i] = Specs::NULL_INDEX;

    node.indirectBlock = Specs::NULL_INDEX;

    addEntryToDirectory(parentIdx, newDirIdx);
}

void FileSystem::rmdir(const std::string& path) {
    if (path.empty()) {
        throw std::runtime_error("[rmdir] Missing operand.");
    }

    int32_t targetIdx = resolvePath(path);

    if (targetIdx == Specs::NULL_INDEX) {
        throw std::runtime_error("[rmdir] No such file or directory.");
    }

    if (targetIdx == 0) {
        throw std::runtime_error("[rmdir] Cannot remove root directory.");
    }

    InodeEntry* entry = controller.getInode(targetIdx);

    if (!entry || !entry->node.isDirectory) {
        throw std::runtime_error("[rmdir] Not a directory.");
    }

    if (!isDirectoryEmpty(targetIdx)) {
        throw std::runtime_error("[rmdir] Directory not empty.");
    }

    unlinkInodeFromParent(targetIdx);

    deleteInodeRecursive(targetIdx);
}

// --- NAVIGATION & MAINTENANCE ---
int32_t FileSystem::resolvePath(const std::string& path) {
    if (path.empty()) return currentDirIdx;

    std::vector<std::string> tokens = StringUtils::tokenize(path);

    // Determine starting point
    int32_t currentIdx = (path[0] == '/') ? 0 : currentDirIdx;

    if (tokens.empty()) return currentIdx;

    for (const std::string& tok : tokens) {
        if (tok == ".") continue;

        InodeEntry* entry = controller.getInode(currentIdx);

        if (!entry || !entry->node.isDirectory) {
            return Specs::NULL_INDEX;
        }

        if (tok == "..") {
            currentIdx = entry->node.parent;

            if (currentIdx == Specs::NULL_INDEX) currentIdx = 0;

            continue;
        }

        // Use our high-speed cached lookup
        int32_t nextIdx = findChildInDirectory(currentIdx, tok);

        if (nextIdx == Specs::NULL_INDEX) {
            // Path segment not found
            return Specs::NULL_INDEX;
        }

        currentIdx = nextIdx;
    }

    return currentIdx;
}

void FileSystem::sync() {
    controller.sync();
}

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