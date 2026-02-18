#include <stdexcept>

#include "path_engine.hpp"
#include "string_utils.hpp"

PathEngine::PathEngine(DiskController& controller, LRUCache<int32_t, Specs::Dentry>& dirCache)
    : controller(controller), dirCache(dirCache) {
}

Specs::PathQuery PathEngine::parsePathQuery(const std::string& path, int32_t currentDirIdx) {
    if (path.empty()) {
        throw std::runtime_error("Missing operand.");
    }

    Specs::PathQuery query{};
    query.isWildcard = (path.find_first_of("*?") != std::string::npos);

    // No wildcards: resolve exactly
    if (!query.isWildcard) {
        query.exactIdx = resolvePath(path, currentDirIdx);

        if (query.exactIdx == Specs::NULL_INDEX) {
            throw std::runtime_error("No such file or directory: " + path);
        }

        return query;
    }

    // Wildcards: split into directory context and the pattern
    size_t lastSlash = path.find_last_of('/');

    if (lastSlash == std::string::npos) {
        query.dirIdx = currentDirIdx;
        query.pattern = path;
    }
    else {
        std::string dirPart = path.substr(0, lastSlash);

        if (dirPart.empty()) {
            dirPart = "/";
        }

        query.dirIdx = resolvePath(dirPart, currentDirIdx);
        query.pattern = path.substr(lastSlash + 1);

        if (query.pattern.empty()) {
            query.pattern = "*";
        }
    }

    if (query.dirIdx == Specs::NULL_INDEX) {
        throw std::runtime_error("Invalid path.");
    }

    return query;
}

int32_t PathEngine::findChildInDirectory(const std::string& name, int32_t dirIdx) {
    Specs::Dentry* dentry = getOrPopulateDentry(dirIdx);

    if (!dentry) {
        return Specs::NULL_INDEX;
    }

    std::unordered_map<std::string, int32_t>::iterator it = dentry->nameToInode.find(name);

    if (it != dentry->nameToInode.end()) {
        return it->second;
    }

    return Specs::NULL_INDEX;
}

int32_t PathEngine::resolvePath(const std::string& path, int32_t currentDirIdx) {
    if (path.empty()) {
        return currentDirIdx;
    }

    std::vector<std::string> tokens = StringUtils::tokenize(path);

    int32_t currentIdx = (path[0] == '/') ? 0 : currentDirIdx;

    if (tokens.empty()) {
        return currentIdx;
    }

    for (const std::string& tok : tokens) {
        if (tok == ".") {
            continue;
        }

        InodeEntry* entry = controller.getInode(currentIdx);

        if (!entry || !entry->node.isDirectory) {
            return Specs::NULL_INDEX;
        }

        if (tok == "..") {
            currentIdx = entry->node.parent;

            if (currentIdx == Specs::NULL_INDEX) {
                currentIdx = 0;
            }

            continue;
        }

        // Use cached lookup
        int32_t nextIdx = findChildInDirectory(tok, currentIdx);

        if (nextIdx == Specs::NULL_INDEX) {
            return Specs::NULL_INDEX;
        }

        currentIdx = nextIdx;
    }

    return currentIdx;
}

std::string PathEngine::getFullPath(int32_t inodeIdx) {
    if (!inodeIdx) {
        return "/";
    }

    if (inodeIdx == Specs::NULL_INDEX) {
        return "";
    }

    int32_t curr = inodeIdx;
    std::vector<std::string> parts;

    while (curr != 0 && curr != Specs::NULL_INDEX) {
        InodeEntry* entry = controller.getInode(curr);

        if (!entry) break;

        parts.push_back(std::string(entry->node.name));

        // Move to parent
        curr = entry->node.parent;
    }

    std::string fullPath = "";

    for (int i = parts.size() - 1; i >= 0; --i) {
        fullPath += "/" + parts[i];
    }

    return fullPath.empty() ? "/" : fullPath;
}

std::vector<int32_t> PathEngine::findAllMatches(const std::string& pattern, int32_t dirIdx) {
    std::vector<int32_t> matches;

    // Ensure directory is loaded into the RAM cache
    Specs::Dentry* dentry = getOrPopulateDentry(dirIdx);

    if (!dentry) {
        return matches;
    }

    // Iterate through the cached name-to-inode map
    std::unordered_map<std::string, int32_t>::iterator it;

    for (it = dentry->nameToInode.begin(); it != dentry->nameToInode.end(); ++it) {
        const std::string& filename = it->first;
        int32_t inodeIdx = it->second;

        if (StringUtils::matchesPattern(pattern, filename)) {
            matches.push_back(inodeIdx);
        }
    }

    return matches;
}

void PathEngine::splitPath(const std::string& path, std::string& parentPath, std::string& name) {
    if (path == "/") {
        parentPath = "/";

        name = "";

        return;
    }

    size_t lastSlash = path.find_last_of('/');

    if (lastSlash == std::string::npos) {
        // No slash: parent is current directory, name is the path itself
        parentPath = ".";

        name = path;
    }
    else if (lastSlash == 0) {
        // Slash at the start: parent is root, name follows
        parentPath = "/";

        name = path.substr(1);
    }
    else {
        // Parent is everything before the last slash, name is everything after
        parentPath = path.substr(0, lastSlash);

        name = path.substr(lastSlash + 1);
    }

    if (name.empty() && parentPath != "/" && parentPath != ".") {
        // Recursive call to strip the trailing slash and split again
        splitPath(parentPath, parentPath, name);
    }
}

Specs::Dentry* PathEngine::getOrPopulateDentry(int32_t dirIdx) {
    Specs::Dentry* cached = dirCache.get(dirIdx);

    if (cached) {
        return cached;
    }

    InodeEntry* entry = controller.getInode(dirIdx);

    if (!entry || !entry->node.isDirectory) {
        return nullptr;
    }

    Specs::Dentry newDentry{};
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