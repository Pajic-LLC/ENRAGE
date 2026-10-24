#include "core/archive.hpp"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: enrage_pack <source-directory> <output.enr>\n";
        return 2;
    }

    const std::filesystem::path source = argv[1];
    const std::filesystem::path output = argv[2];
    if (!enrage::core::Archive::create(source, output)) {
        std::cerr << "failed to create package\n";
        return 1;
    }

    std::cout << output.string() << '\n';
    return 0;
}
