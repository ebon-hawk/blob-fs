#ifndef SPECS_HPP
#define SPECS_HPP

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Specs {
    // --- SYSTEM CONSTANTS ---
    constexpr int32_t  NULL_INDEX = -1;
    constexpr uint32_t BLOCK_SIZE = 4096;
    constexpr uint32_t DIRECT_BLOCKS_COUNT = 32;
    constexpr uint32_t INODE_SIZE = 256;
    constexpr uint32_t MAGIC_NUMBER = 0x424C4F42;
    constexpr uint32_t MAX_NAME_LEN = 64;

    // --- PADDING CONSTANTS ---
    constexpr uint32_t INODE_PADDING_SIZE = 36;
    constexpr uint32_t SB_PADDING_SIZE = 20;

    struct Superblock {
        // Unique signature to identify the file as a valid "BLOB" system
        uint32_t magicNumber;

        // Total physical file size limit
        uint64_t maxDiskSize;

        // Max number of directories/files allowed
        uint32_t inodeCount;

        // Total number of addressable blocks in the data region
        uint32_t blockCount;

        // Size of a single block in the data region
        uint32_t blockSize;

        // Byte offset where the inode usage bitmap starts
        uint32_t inodeBitmapOffset;

        // Byte offset where the block usage bitmap starts
        uint32_t blockBitmapOffset;

        // Absolute file offset to the array of fixed-size inode structures
        uint32_t inodeTableOffset;

        // Absolute file offset to where the actual file content begins
        uint32_t dataRegionOffset;

        // Reserved space for future expansion (or alignment to a 64-byte header size)
        uint8_t padding[SB_PADDING_SIZE];
    };

    struct Inode {
        char name[MAX_NAME_LEN];
        uint8_t isDirectory;
        uint32_t size;

        int32_t parent;

        int32_t firstChild;
        int32_t nextSibling;
        int32_t prevSibling;

        int32_t directBlocks[DIRECT_BLOCKS_COUNT];
        int32_t indirectBlock;
        uint8_t padding[INODE_PADDING_SIZE];
    };

    // Helper to calculate bitmap size in bytes
    inline uint32_t bitsToBytes(uint32_t bits) {
        return (bits + 7) / 8;
    }

    struct PathQuery {
        // Filename or wildcard
        std::string pattern = "*";

        // Valid only if not a wildcard
        int32_t exactIdx = Specs::NULL_INDEX;

        // Directory to search in
        int32_t dirIdx = Specs::NULL_INDEX;

        // Whether wildcard logic was used
        bool isWildcard = false;
    };

    struct Dentry {
        // Maps filename to inode index
        std::unordered_map<std::string, int32_t> nameToInode;
    };
}

#endif