#include "adapters/null_adapter.hpp"
#include "core/runtime.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

int main(int argc, char** argv) {
    if (argc != 3) return 2;

#ifdef _WIN32
    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
#endif

    const std::filesystem::path root = argv[1];
    const std::filesystem::path plugin = argv[2];
    const auto mods = root / "mods";
    const auto mod = mods / "fault";
    const auto bin = mod / "bin";

    std::filesystem::remove_all(root);
    std::filesystem::create_directories(bin);
    const auto installed_plugin = bin / plugin.filename();
    std::filesystem::copy_file(plugin, installed_plugin, std::filesystem::copy_options::overwrite_existing);

    std::ofstream manifest{mod / "mod.json"};
    manifest << "{\"id\":\"example.crash\",\"nativePlugins\":[\"bin/"
             << installed_plugin.filename().string() << "\"]}";
    manifest.close();

    enrage::core::Runtime runtime{
        std::make_unique<enrage::adapters::NullAdapter>(), mods};
    if (!runtime.start()) return 3;
    if (!runtime.extensions_active()) return 4;
    if (runtime.plugins().plugins().size() != 1) return 5;

    runtime.tick(1.0 / 60.0);
    return 99;
}
