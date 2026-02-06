#ifndef DISK_CONTROLLER_HPP
#define DISK_CONTROLLER_HPP

#include <fstream>

#include "specs.hpp"

/**
 * @brief Retrieves an inode from RAM cache if available, otherwise fetches from disk.
 * @return The requested inode.
 */
Specs::Inode getInode(std::fstream& fs, const Specs::Superblock& sb, int32_t idx);

/**
 * @brief Saves an inode to disk and updates its entry in the RAM cache.
 * @note Use this for all metadata changes (renaming, resizing, linking).
 */
void updateInode(std::fstream& fs, const Specs::Superblock& sb, int32_t idx, const Specs::Inode& node);

/**
 * @brief Scans the inode table for an unused slot (name[0] == '\0').
 * @return Index of available inode, or -1 if the table is full.
 */
int32_t findFreeInode(std::fstream& fs, const Specs::Superblock& sb);

/**
 * @brief Scans the bitmap to find the first available 4KB data block.
 * @return Index of the free block, or -1 if the disk is full.
 */
int32_t findFreeBlock(std::fstream& fs, Specs::Superblock& sb);

/**
 * @brief Raw read of an inode from the disk's inode table.
 */
Specs::Inode readInode(std::fstream& fs, const Specs::Superblock& sb, int32_t idx);

/**
 * @brief Raw write of an inode to a specific index on disk.
 */
void writeInode(std::fstream& fs, const Specs::Superblock& sb, int32_t idx, const Specs::Inode& node);

/**
 * @brief Reads a raw 4KB block from the data region into a buffer.
 */
void readBlock(std::fstream& fs, const Specs::Superblock& sb, int32_t blockIdx, char* buffer);

/**
 * @brief Writes a raw 4KB block from a buffer to the data region.
 */
void writeBlock(std::fstream& fs, const Specs::Superblock& sb, int32_t blockIdx, const char* buffer);

// TODO: Write description
Specs::Superblock initNewDisk(std::fstream& fs, uint64_t maxSize);

#endif // DISK_CONTROLLER_HPP