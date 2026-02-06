#ifndef SPECS_HPP
#define SPECS_HPP

#include <cstdint>

namespace Specs {
    constexpr int32_t  NULL_INDEX = -1;
    constexpr uint32_t BLOCK_SIZE = 4096;
    constexpr uint32_t INODE_SIZE = 256;
    constexpr uint32_t MAGIC_NUMBER = 0x5346;

    struct Superblock {
        uint32_t magicNumber;

        // Size of a single data block in bytes
        uint32_t blockSize;

        // Maximum allowed size for a single file in bytes
        uint32_t maxFileSize;

        // The total capacity of the inode table
        uint32_t inodeCount;

        // The total number of data blocks available for file content;
        // matches the number of bits in the data block bitmap
        uint32_t dataBlockCount;

        // Byte offset where the inode usage bitmap starts
        uint32_t inodeBitmapOffset;

        // Byte offset where the data block usage bitmap starts
        uint32_t blockBitmapOffset;

        // Byte offset where the array of inode structs begins
        uint32_t inodeTableOffset;

        // Byte offset where the raw file data storage begins
        uint32_t dataRegionOffset;
    };

    struct Inode {
        char name[64];
        uint8_t isDirectory;
        uint32_t size;

        int32_t parent;

        int32_t firstChild;
        int32_t nextSibling;
        int32_t prevSibling;

        int32_t directBlocks[32];
        int32_t indirectBlock;

        uint8_t padding[36];
    };

    static_assert(sizeof(Inode) == INODE_SIZE, "Inode must be exactly 256 bytes.");
}

#endif // SPECS_HPP