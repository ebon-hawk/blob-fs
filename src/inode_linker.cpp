#include <cstring>

#include "inode_linker.hpp"

InodeLinker::InodeLinker(DiskController& controller, LRUCache<int32_t, Specs::Dentry>& dirCache)
    : controller(controller), dirCache(dirCache) {
}

bool InodeLinker::isDirectoryEmpty(int32_t dirIdx) {
    InodeEntry* entry = controller.getInode(dirIdx);

    if (!entry || !entry->node.isDirectory) {
        return false;
    }

    return entry->node.firstChild == Specs::NULL_INDEX;
}

void InodeLinker::deleteRecursive(int32_t inodeIdx) {
    if (!inodeIdx || inodeIdx == Specs::NULL_INDEX) {
        // Never recursively delete the root
        return;
    }

    InodeEntry* entry = controller.getInode(inodeIdx);

    if (!entry) {
        return;
    }

    Specs::Inode& node = entry->node;

    // If it's a directory, clean out the children first
    if (node.isDirectory) {
        int32_t currentChildIdx = node.firstChild;

        while (currentChildIdx != Specs::NULL_INDEX) {
            // We must fetch the sibling pointer BEFORE deleting the child
            InodeEntry* childEntry = controller.getInode(currentChildIdx);

            if (!childEntry) {
                break;
            }

            int32_t nextSiblingIdx = childEntry->node.nextSibling;

            // Recurse down
            deleteRecursive(currentChildIdx);

            currentChildIdx = nextSiblingIdx;
        }

        // Since the directory is being deleted, its name-map is now garbage
        dirCache.remove(inodeIdx);
    }

    try {
        controller.freeInode(inodeIdx);
    }
    catch (...) {
        throw;
    }
}

void InodeLinker::link(int32_t childIdx, int32_t parentIdx) {
    InodeEntry* parentEntry = controller.getInode(parentIdx);

    InodeEntry* childEntry = controller.getInode(childIdx);

    if (!childEntry || !parentEntry) return;

    Specs::Inode childNode = childEntry->node;
    Specs::Inode parentNode = parentEntry->node;

    // Save old sibling pointers for rollback
    int32_t oldChildNext = childNode.nextSibling;
    int32_t oldChildPrev = childNode.prevSibling;
    int32_t oldParentFirstChild = parentNode.firstChild;

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

    try {
        controller.updateInode(parentIdx, parentNode);

        controller.updateInode(childIdx, childNode);

        // If this directory's name-map is in RAM, we must add the new entry immediately
        Specs::Dentry* cachedDir = dirCache.get(parentIdx);

        if (cachedDir) {
            cachedDir->nameToInode[childNode.name] = childIdx;
        }
    }
    catch (...) {
        // Rollback in memory
        parentNode.firstChild = oldParentFirstChild;

        childNode.nextSibling = oldChildNext;
        childNode.prevSibling = oldChildPrev;
        controller.updateInode(parentIdx, parentNode);

        controller.updateInode(childIdx, childNode);

        throw;
    }
}

void InodeLinker::move(int32_t inodeIdx, int32_t newParentIdx, const std::string& newName) {
    InodeEntry* inodeEntry = controller.getInode(inodeIdx);
    InodeEntry* newParentEntry = controller.getInode(newParentIdx);
    InodeEntry* oldParentEntry = controller.getInode(inodeEntry->node.parent);

    if (!inodeEntry || !newParentEntry) {
        throw std::runtime_error("Invalid inode or parent.");
    }

    // Store old state for rollback
    Specs::Inode oldNode = inodeEntry->node;
    Specs::Inode oldParentNode = oldParentEntry->node;

    try {
        unlink(inodeIdx);

        if (!newName.empty()) {
            std::strncpy(inodeEntry->node.name, newName.c_str(), sizeof(inodeEntry->node.name) - 1);

            inodeEntry->node.name[sizeof(inodeEntry->node.name) - 1] = '\0';
        }

        link(inodeIdx, newParentIdx);
    }
    catch (...) {
        // Rollback
        controller.updateInode(oldParentEntry->node.parent, oldParentNode);

        controller.updateInode(inodeIdx, oldNode);

        throw;
    }
}

void InodeLinker::unlink(int32_t targetIdx) {
    InodeEntry* targetEntry = controller.getInode(targetIdx);

    if (!targetEntry) {
        return;
    }

    Specs::Inode& targetNode = targetEntry->node;
    int32_t pIdx = targetNode.parent;

    if (pIdx == Specs::NULL_INDEX) {
        return;
    }

    InodeEntry* parentEntry = controller.getInode(pIdx);

    if (!parentEntry) {
        return;
    }

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

    Specs::Dentry* cachedDir = dirCache.get(pIdx);

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