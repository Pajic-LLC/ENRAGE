#include "adapters/null_adapter.hpp"
#include "core/archive.hpp"
#include "core/runtime.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path plugin_path = argv[1];

    const auto root = std::filesystem::temp_directory_path() / "enrage-vfs-integration-test";
    const auto mods = root / "mods";
    const auto source = root / "source";
    const auto package = mods / "probe.enr";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(source / "files" / "vfs");
    std::filesystem::create_directories(source / "bin");

    const auto plugin_name = plugin_path.filename();
    std::filesystem::copy_file(plugin_path, source / "bin" / plugin_name);
    std::ofstream{source / "files" / "vfs" / "probe.txt"} << "hello";

    std::ofstream manifest{source / "mod.json"};
    manifest << "{\"id\":\"example.vfs\",\"nativePlugins\":[\"bin/"
             << plugin_name.generic_string() << "\"]}";
    manifest.close();

    assert(enrage::core::Archive::create(source, package));

    enrage::core::Runtime runtime{
        std::make_unique<enrage::adapters::NullAdapter>(),
        mods};
    assert(runtime.start());
    assert(runtime.running());
    assert(runtime.extensions_active());
    assert(runtime.loaded_mods().size() == 1);
    assert(runtime.loaded_mods()[0].kind == enrage::core::PackageKind::archive);
    assert(runtime.plugins().plugins().size() == 1);
    assert(runtime.plugins().plugins()[0].id == "dev.enrage.vfs-probe");
    assert(runtime.overlay().read_text("vfs/probe.txt") == "hello");

    // Only the OS-loadable native module is materialized. Content stays inside
    // the archive and is served through Overlay/VFS.
    assert(std::filesystem::is_regular_file(runtime.loaded_mods()[0].root / "bin" / plugin_name));
    assert(!std::filesystem::exists(runtime.loaded_mods()[0].root / "files" / "vfs" / "probe.txt"));

    runtime.stop();
    std::filesystem::remove_all(root);
    return 0;
}
