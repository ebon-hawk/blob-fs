#ifndef INODE_LINKER_HPP
#define INODE_LINKER_HPP

#include "disk_controller.hpp"
#include "lru_cache.hpp"
#include "specs.hpp"

class InodeLinker {
public:
    InodeLinker(DiskController& controller, LRUCache<int32_t, Specs::Dentry>& dirCache);

    bool isDirectoryEmpty(int32_t dirIdx);
    void deleteRecursive(int32_t inodeIdx);
    void link(int32_t childIdx, int32_t parentIdx);
    void move(int32_t inodeIdx, int32_t newParentIdx, const std::string& newName);
    void unlink(int32_t targetIdx);

private:
    DiskController& controller;
    LRUCache<int32_t, Specs::Dentry>& dirCache;
};

#endif