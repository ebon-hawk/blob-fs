#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "file_system.hpp"
#include "specs.hpp"

void printPrompt(FileSystem& sys, const std::string& filename) {
    std::cout << "[" << filename << ":" << sys.getCurrentPath() << "]$ ";
}

int main() {
    std::fstream diskFile;
    std::string diskName;

    Specs::Superblock sb;

    std::cout << "=== BLOB FILE SYSTEM INTERFACE ===\n";

    std::cout << "Enter virtual disk filename: ";

    if (!std::getline(std::cin, diskName) || diskName.empty()) {
        std::cerr << "Invalid filename. Exiting...\n";

        return 1;
    }

    // Try to open existing file
    diskFile.open(diskName, std::ios::in | std::ios::out | std::ios::binary);

    try {
        if (!diskFile.is_open()) {
            // Create new FS if it doesn't exist
            uint64_t maxSize = 0;

            std::cout << "Disk not found. Creating new file system. Enter max size (bytes): ";

            if (!(std::cin >> maxSize)) return 1;

            std::cin.ignore();

            diskFile.open(diskName, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
            sb = FileSystem::createFresh(diskFile, maxSize);
        }
        else {
            // Load existing superblock from the start of the file
            diskFile.read(reinterpret_cast<char*>(&sb), sizeof(Specs::Superblock));
        }

        FileSystem sys(diskFile, sb);
        std::string line;

        while (true) {
            printPrompt(sys, diskName);

            if (!std::getline(std::cin, line) || line == "exit") {
                break;
            }

            if (line.empty()) {
                continue;
            }

            std::stringstream ss(line);

            std::string cmd;

            ss >> cmd;

            try {
                if (cmd == "cat") {
                    std::string path;

                    if (ss >> path) {
                        sys.cat(path);
                    }
                }
                else if (cmd == "cd") {
                    std::string path;

                    if (ss >> path) {
                        sys.cd(path);
                    }
                    else {
                        sys.cd("/");
                    }
                }
                else if (cmd == "cp") {
                    std::string dest, src;

                    if (ss >> src >> dest) {
                        sys.cp(src, dest);
                    }
                }
                else if (cmd == "export") {
                    std::string hDest, vSrc;

                    if (ss >> vSrc >> hDest) {
                        sys.exportFile(vSrc, hDest);
                    }
                }
                else if (cmd == "import") {
                    std::string hSrc, opt, vDest;

                    if (ss >> hSrc >> vDest) {
                        ss >> opt;
                        sys.importFile(hSrc, vDest, (opt == "+append"));
                    }
                }
                else if (cmd == "ls") {
                    // Default to current directory
                    std::string path = ".";

                    ss >> path;
                    sys.ls(path);
                }
                else if (cmd == "mkdir") {
                    std::string path;

                    if (ss >> path) {
                        sys.mkdir(path);
                    }
                }
                else if (cmd == "rm") {
                    std::string path;

                    if (ss >> path) {
                        sys.rm(path);
                    }
                }
                else if (cmd == "rmdir") {
                    std::string path;

                    if (ss >> path) {
                        sys.rmdir(path);
                    }
                }
                else if (cmd == "help") {
                    std::cout << "AVAILABLE: cat, cd, cp, export, import, ls, mkdir, rm, rmdir, help, exit\n";
                }
                else {
                    std::cerr << "UNKNOWN COMMAND: " << cmd << "\n";
                }
            }
            catch (const std::exception& e) {
                std::cerr << "ERROR: " << e.what() << std::endl;
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "CRITICAL ERROR: " << e.what() << std::endl;

        return 1;
    }

    diskFile.close();

    return 0;
}