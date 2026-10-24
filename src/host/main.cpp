#include "adapters/null_adapter.hpp"
#include "core/runtime.hpp"

#include <filesystem>
#include <iostream>
#include <memory>

int main(int argc, char** argv) {
    std::filesystem::path mod_root = "mods";
    if (argc > 1) {
        mod_root = argv[1];
    }

    enrage::core::Runtime runtime{
        std::make_unique<enrage::adapters::NullAdapter>(),
        std::move(mod_root),
    };

    if (!runtime.start()) {
        std::cerr << "ENRAGE failed to initialize.\n";
        return 1;
    }

    std::cout << "ENRAGE runtime: " << runtime.adapter().name() << '\n';
    std::cout << "Discovered " << runtime.mods().candidates().size() << " mod candidate(s).\n";

    for (const auto& mod : runtime.mods().candidates()) {
        std::cout << "  " << mod.id << (mod.packaged ? " [package]" : " [directory]") << '\n';
    }

    runtime.stop();
    return 0;
}
