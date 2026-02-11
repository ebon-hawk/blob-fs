#include <iostream>
#include <stdexcept>

#include "file_system.hpp"

const size_t FileSystem::DIR_CACHE_CAPACITY = 64;

FileSystem::FileSystem(std::fstream& fs, const Specs::Superblock& sb)
    : fs(fs),
    sb(sb),
    controller(fs, sb),
    dirCache(DIR_CACHE_CAPACITY),
    linker(controller, dirCache),
    pathEngine(controller, dirCache) {
}

FileSystem::~FileSystem() {
    try {
        controller.sync();
    }
    catch (...) {}
}

Specs::Superblock FileSystem::createFresh(std::fstream& fs, uint64_t maxDiskSize) {
    if (!fs) {
        throw std::runtime_error("Invalid file stream.");
    }

    if (maxDiskSize < sizeof(Specs::Superblock) + Specs::BLOCK_SIZE) {
        throw std::runtime_error("Disk size too small.");
    }

    Specs::Superblock sb{};
    sb.blockSize = Specs::BLOCK_SIZE;
    sb.magicNumber = Specs::MAGIC_NUMBER;
    sb.maxDiskSize = maxDiskSize;

    const uint32_t totalBlocksPossible =
        static_cast<uint32_t>(maxDiskSize / sb.blockSize);

    if (totalBlocksPossible < 8) {
        throw std::runtime_error("Disk too small for block structure.");
    }

    // Inode count heuristic (25%)
    sb.inodeCount = totalBlocksPossible / 4;

    if (sb.inodeCount < 16) {
        sb.inodeCount = 16;
    }

    uint64_t offset = sizeof(Specs::Superblock);

    // Inode bitmap
    sb.inodeBitmapOffset = static_cast<uint32_t>(offset);

    offset += (sb.inodeCount + 7) / 8;

    // Block bitmap
    sb.blockBitmapOffset = static_cast<uint32_t>(offset);

    offset += (totalBlocksPossible + 7) / 8;

    // Align to 64 bytes (clean boundary)
    if (offset % 64 != 0) {
        offset += (64 - (offset % 64));
    }

    // Inode table
    sb.inodeTableOffset = static_cast<uint32_t>(offset);

    offset += static_cast<uint64_t>(sb.inodeCount) * sizeof(Specs::Inode);

    // Align to block boundary before data region
    if (offset % sb.blockSize != 0) {
        offset += (sb.blockSize - (offset % sb.blockSize));
    }

    sb.dataRegionOffset = static_cast<uint32_t>(offset);

    if (sb.dataRegionOffset >= maxDiskSize) {
        throw std::runtime_error("Metadata exceeds disk size.");
    }

    // Data block count
    sb.blockCount = static_cast<uint32_t>(
        (maxDiskSize - sb.dataRegionOffset) / sb.blockSize
        );

    if (sb.blockCount == 0) {
        throw std::runtime_error("No usable data blocks.");
    }

    // Resize physical file
    fs.seekp(maxDiskSize - 1);

    char zero = 0;
    fs.write(&zero, 1);

    fs.flush();

    // Zero metadata region only (not full disk)
    fs.seekp(0);

    const uint64_t metadataSize = sb.dataRegionOffset;
    std::vector<char> zeroBuf(Specs::BLOCK_SIZE, 0);
    uint64_t written = 0;

    while (written < metadataSize) {
        uint64_t chunk = std::min<uint64_t>(zeroBuf.size(), metadataSize - written);

        fs.write(zeroBuf.data(), chunk);
        written += chunk;
    }

    // Write superblock
    fs.seekp(0);
    fs.write(reinterpret_cast<const char*>(&sb), sizeof(Specs::Superblock));

    // Mark inode 0 as used
    fs.seekp(sb.inodeBitmapOffset);
    uint8_t firstByte = 0x01;

    fs.write(reinterpret_cast<char*>(&firstByte), 1);

    // Initialize root inode
    Specs::Inode root{};
    root.isDirectory = true;
    root.size = 0;

    root.parent = Specs::NULL_INDEX;

    root.firstChild = root.nextSibling = root.prevSibling = Specs::NULL_INDEX;
    std::strncpy(root.name, "/", Specs::MAX_NAME_LEN - 1);

    root.name[Specs::MAX_NAME_LEN - 1] = '\0';

    for (uint32_t i = 0; i < Specs::DIRECT_BLOCKS_COUNT; ++i) {
        root.directBlocks[i] = Specs::NULL_INDEX;
    }

    root.indirectBlock = Specs::NULL_INDEX;

    fs.seekp(sb.inodeTableOffset);
    fs.write(reinterpret_cast<const char*>(&root), sizeof(Specs::Inode));

    fs.flush();

    return sb;
}

std::string FileSystem::getCurrentPath() {
    return pathEngine.getFullPath(currentDirIdx);
}

// --- PUBLIC SHELL COMMANDS ---
void FileSystem::cd(const std::string& path) {
    if (path.empty()) {
        std::cout << pathEngine.getFullPath(currentDirIdx) << std::endl;

        return;
    }

    if (path == "~") {
        updateCurrentDir(0);

        return;
    }

    if (path == "-") {
        if (prevDirIdx == Specs::NULL_INDEX) {
            throw std::runtime_error("[cd] OLDPWD not set.");
        }

        std::cout << pathEngine.getFullPath(prevDirIdx) << std::endl;
        updateCurrentDir(prevDirIdx);

        return;
    }

    Specs::PathQuery query = pathEngine.parsePathQuery(path, currentDirIdx);

    if (query.isWildcard) {
        throw std::runtime_error("[cd] Wildcards not supported for directory navigation.");
    }

    updateCurrentDir(query.exactIdx);
}

void FileSystem::ls(const std::string& path) {
    std::vector<int32_t> targets;

    if (path.empty()) {
        targets = pathEngine.findAllMatches("*", currentDirIdx);
    }
    else {
        Specs::PathQuery query = pathEngine.parsePathQuery(path, currentDirIdx);

        if (query.isWildcard) {
            targets = pathEngine.findAllMatches(query.pattern, query.dirIdx);
        }
        else {
            InodeEntry* entry = controller.getInode(query.exactIdx);

            if (entry && entry->node.isDirectory) {
                targets = pathEngine.findAllMatches("*", query.exactIdx);
            }
            else if (entry) {
                targets.push_back(query.exactIdx);
            }
        }
    }

    if (targets.empty()) {
        return;
    }

    std::cout << "TYPE\tSIZE\tNAME\n";

    std::cout << "------------------------\n";

    for (int32_t idx : targets) {
        InodeEntry* entry = controller.getInode(idx);

        if (!entry) {
            continue;
        }

        std::string type = entry->node.isDirectory ? "DIR" : "FILE";

        std::cout << type << "\t"
            << entry->node.size << "\t"
            << entry->node.name << "\n";
    }
}

void FileSystem::mkdir(const std::string& path) {
    std::string dirName, parentPath;

    pathEngine.splitPath(path, parentPath, dirName);

    if (!isValidFilename(dirName)) {
        throw std::runtime_error("[mkdir] Invalid directory name.");
    }

    int32_t pIdx = pathEngine.resolvePath(parentPath, currentDirIdx);

    if (pIdx == Specs::NULL_INDEX) {
        throw std::runtime_error("[mkdir] Parent directory does not exist.");
    }

    int32_t newIdx = controller.createInode(pIdx, dirName, true);

    if (newIdx == Specs::NULL_INDEX) {
        throw std::runtime_error("[mkdir] Disk full or limit reached.");
    }

    linker.link(newIdx, pIdx);

    // Persist the new directory and the link
    controller.sync();
}

void FileSystem::rmdir(const std::string& path) {
    Specs::PathQuery query = pathEngine.parsePathQuery(path, currentDirIdx);

    std::vector<int32_t> targets = query.isWildcard
        ? pathEngine.findAllMatches(query.pattern, query.dirIdx)
        : std::vector<int32_t>{ query.exactIdx };

    if (targets.empty()) {
        throw std::runtime_error("[rmdir] No directories matched the criteria.");
    }

    size_t removedCount = 0;

    for (int32_t idx : targets) {
        if (idx == 0) {
            throw std::runtime_error("[rmdir] Cannot remove root directory.");
        }

        InodeEntry* entry = controller.getInode(idx);

        if (!entry) {
            continue;
        }

        if (!entry->node.isDirectory) {
            if (!query.isWildcard) {
                throw std::runtime_error("[rmdir] Not a directory: " + std::string(entry->node.name));
            }

            continue;
        }

        if (!linker.isDirectoryEmpty(idx)) {
            if (!query.isWildcard) {
                throw std::runtime_error("[rmdir] Directory not empty: " + std::string(entry->node.name));
            }

            continue;
        }

        linker.unlink(idx);

        controller.freeInode(idx);

        ++removedCount;
    }

    if (removedCount == 0 && !query.isWildcard) {
        throw std::runtime_error("[rmdir] Failed to remove directory.");
    }

    if (removedCount > 0) {
        controller.sync();
    }
}

void FileSystem::cat(const std::string& path) {
    Specs::PathQuery query = pathEngine.parsePathQuery(path, currentDirIdx);

    std::vector<int32_t> targets = query.isWildcard
        ? pathEngine.findAllMatches(query.pattern, query.dirIdx)
        : std::vector<int32_t>{ query.exactIdx };

    if (targets.empty()) {
        throw std::runtime_error("[cat] No files matched.");
    }

    for (int32_t idx : targets) {
        InodeEntry* entry = controller.getInode(idx);

        if (!entry || entry->node.isDirectory) {
            continue;
        }

        if (targets.size() > 1) {
            std::cout << "==> " << entry->node.name << " <==" << std::endl;
        }

        printFileContents(idx);
        std::cout << std::endl;
    }
}

void FileSystem::cp(const std::string& srcPath, const std::string& destPath) {
    Specs::PathQuery query = pathEngine.parsePathQuery(srcPath, currentDirIdx);

    std::vector<int32_t> sources = query.isWildcard
        ? pathEngine.findAllMatches(query.pattern, query.dirIdx)
        : std::vector<int32_t>{ query.exactIdx };

    if (sources.empty()) {
        throw std::runtime_error("[cp] No files matched source pattern.");
    }

    int32_t destIdx = pathEngine.resolvePath(destPath, currentDirIdx);

    bool destExists = destIdx != Specs::NULL_INDEX;
    bool destIsDir = destExists && controller.getInode(destIdx)->node.isDirectory;

    if (sources.size() > 1 && !destIsDir) {
        throw std::runtime_error("[cp] Destination must be a directory when copying multiple files.");
    }

    for (int32_t srcIdx : sources) {
        InodeEntry* srcEntry = controller.getInode(srcIdx);

        if (!srcEntry) {
            continue;
        }

        if (srcEntry->node.isDirectory) {
            continue;
        }

        int32_t finalParent;
        std::string finalName;

        if (destIsDir) {
            finalParent = destIdx;

            finalName = srcEntry->node.name;
        }
        else if (destExists) {
            throw std::runtime_error("[cp] Destination file exists: " + destPath);
        }
        else {
            std::string parentStr;

            pathEngine.splitPath(destPath, parentStr, finalName);

            finalParent = parentStr.empty() ? currentDirIdx : pathEngine.resolvePath(parentStr, currentDirIdx);

            if (finalParent == Specs::NULL_INDEX) {
                throw std::runtime_error("[cp] Destination parent directory does not exist: " + parentStr);
            }
        }

        int32_t newIdx = controller.copyInode(srcIdx, finalParent, finalName);

        if (newIdx == Specs::NULL_INDEX) {
            throw std::runtime_error("[cp] Copy failed.");
        }

        linker.link(newIdx, finalParent);
    }

    controller.sync();
}

void FileSystem::rm(const std::string& path) {
    Specs::PathQuery query = pathEngine.parsePathQuery(path, currentDirIdx);
    std::vector<int32_t> targets;

    if (query.isWildcard) {
        targets = pathEngine.findAllMatches(query.pattern, query.dirIdx);
    }
    else {
        targets.push_back(query.exactIdx);
    }

    if (targets.empty()) {
        throw std::runtime_error("[rm] No files matched the criteria.");
    }

    size_t deletedCount = 0;

    for (int32_t idx : targets) {
        InodeEntry* entry = controller.getInode(idx);

        if (!entry) {
            continue;
        }

        if (entry->node.isDirectory) {
            if (!query.isWildcard) {
                throw std::runtime_error("[rm] Cannot remove directory: " + std::string(entry->node.name));
            }

            continue;
        }

        linker.unlink(idx);

        controller.freeInode(idx);

        ++deletedCount;
    }

    if (deletedCount == 0 && !query.isWildcard) {
        throw std::runtime_error("[rm] Failed to delete target.");
    }

    if (deletedCount > 0) {
        // Persist freed bitmaps and unlinked parent blocks
        controller.sync();
    }
}

// --- HOST OS INTERACTION ---
void FileSystem::exportFile(const std::string& srcPath, const std::string& hostDest) {
    int32_t srcIdx = pathEngine.resolvePath(srcPath, currentDirIdx);

    if (srcIdx == Specs::NULL_INDEX) {
        throw std::runtime_error("[export] Source file not found: " + srcPath);
    }

    InodeEntry* entry = controller.getInode(srcIdx);

    if (entry->node.isDirectory) {
        throw std::runtime_error("[export] Cannot export a directory.");
    }

    std::ofstream hostFile(hostDest, std::ios::binary);

    if (!hostFile) {
        throw std::runtime_error("[export] Failed to create host file: " + hostDest);
    }

    uint32_t logicalIdx = 0;
    uint32_t remaining = entry->node.size;

    std::vector<char> buffer(sb.blockSize);

    while (remaining > 0) {
        // Translate logical file position to physical disk block
        int32_t pBlock = getPhysicalBlock(entry->node, logicalIdx++);

        if (pBlock == Specs::NULL_INDEX) {
            throw std::runtime_error("[export] Unexpected end of file (FS corruption).");
        }

        uint32_t toWrite = std::min(remaining, (uint32_t)sb.blockSize);

        controller.readBlock(pBlock, buffer.data());
        hostFile.write(buffer.data(), toWrite);
        remaining -= toWrite;
    }

    hostFile.flush();
}

void FileSystem::importFile(const std::string& hostSrc, const std::string& destPath, bool append) {
    std::ifstream hostFile(hostSrc, std::ios::binary | std::ios::ate);

    if (!hostFile) {
        throw std::runtime_error("[import] Host file not found.");
    }

    std::string fileName, parentPath;
    uint32_t hostSize = static_cast<uint32_t>(hostFile.tellg());

    hostFile.seekg(0, std::ios::beg);
    pathEngine.splitPath(destPath, parentPath, fileName);

    int32_t pIdx = pathEngine.resolvePath(parentPath, currentDirIdx);

    if (pIdx == Specs::NULL_INDEX) {
        throw std::runtime_error("[import] Parent not found.");
    }

    int32_t inodeIdx = pathEngine.findChildInDirectory(fileName, pIdx);

    if (inodeIdx == Specs::NULL_INDEX) {
        if (!isValidFilename(fileName)) {
            throw std::runtime_error("[import] Invalid filename.");
        }

        inodeIdx = controller.createInode(pIdx, fileName, false);
        linker.link(inodeIdx, pIdx);
    }
    else if (!append) {
        throw std::runtime_error("[import] File exists. Use '+append'.");
    }

    InodeEntry* entry = controller.getInode(inodeIdx);

    if (!controller.canGrowFile(entry->node, hostSize)) {
        throw std::runtime_error("[import] No space available.");
    }

    uint32_t oldSize = entry->node.size;

    controller.extendInode(entry->node, oldSize + hostSize);

    // Stream the data into the reserved blocks
    std::vector<char> buffer(sb.blockSize);
    uint32_t bytesWritten = 0;

    while (bytesWritten < hostSize) {
        uint32_t currentPos = oldSize + bytesWritten;
        uint32_t logicalIdx = currentPos / sb.blockSize;

        uint32_t blockOffset = currentPos % sb.blockSize;
        uint32_t toWrite = std::min((uint32_t)sb.blockSize - blockOffset, hostSize - bytesWritten);

        int32_t pBlock = getPhysicalBlock(entry->node, logicalIdx);

        if (blockOffset > 0 || toWrite < sb.blockSize) {
            controller.readBlock(pBlock, buffer.data());
        }

        hostFile.read(buffer.data() + blockOffset, toWrite);

        controller.writeBlock(pBlock, buffer.data());

        bytesWritten += toWrite;
    }

    controller.updateInode(inodeIdx, entry->node);

    controller.sync();
}

// --- FILE VALIDATION & HELPERS ---
bool FileSystem::isValidFilename(const std::string& filename) const {
    if (filename.empty() || filename.length() >= Specs::MAX_NAME_LEN) {
        return false;
    }

    // Check for reserved characters
    const std::string illegalChars = "/\\:*?\"<>|";

    if (filename.find_first_of(illegalChars) != std::string::npos) {
        return false;
    }

    // Prevent "." and ".." as manual names
    if (filename == "." || filename == "..") {
        return false;
    }

    return true;
}

int32_t FileSystem::getPhysicalBlock(const Specs::Inode& node, uint32_t logicalIdx) {
    if (logicalIdx < Specs::DIRECT_BLOCKS_COUNT) {
        return node.directBlocks[logicalIdx];
    }

    uint32_t indirectPos = logicalIdx - Specs::DIRECT_BLOCKS_COUNT;
    uint32_t ptrsPerBlock = sb.blockSize / sizeof(int32_t);

    if (node.indirectBlock == Specs::NULL_INDEX || indirectPos >= ptrsPerBlock) {
        return Specs::NULL_INDEX;
    }

    std::vector<int32_t> table(ptrsPerBlock);

    controller.readBlock(node.indirectBlock, reinterpret_cast<char*>(table.data()));

    return table[indirectPos];
}

void FileSystem::printFileContents(int32_t inodeIdx) {
    InodeEntry* entry = controller.getInode(inodeIdx);

    if (!entry || entry->node.isDirectory) {
        return;
    }

    uint32_t logicalIdx = 0;
    uint32_t remaining = entry->node.size;

    std::vector<char> buffer(sb.blockSize);

    while (remaining > 0) {
        int32_t pBlock = getPhysicalBlock(entry->node, logicalIdx++);

        if (pBlock == Specs::NULL_INDEX) {
            break;
        }

        uint32_t toRead = std::min(remaining, (uint32_t)sb.blockSize);

        controller.readBlock(pBlock, buffer.data());
        std::cout.write(buffer.data(), toRead);

        remaining -= toRead;
    }
}

void FileSystem::updateCurrentDir(int32_t dirIdx) {
    InodeEntry* entry = controller.getInode(dirIdx);

    if (!entry) {
        throw std::runtime_error("Target directory index does not exist.");
    }

    if (!entry->node.isDirectory) {
        throw std::runtime_error("Target is not a directory.");
    }

    if (currentDirIdx != dirIdx) {
        prevDirIdx = currentDirIdx;

        currentDirIdx = dirIdx;
    }
}