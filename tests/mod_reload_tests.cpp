#include "adapters/null_adapter.hpp"
#include "core/archive.hpp"
#include "core/package.hpp"
#include "core/runtime.hpp"
#include "core/status.hpp"

#include <atomic>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace {

bool has_diagnostic(
    const enrage::core::Runtime& runtime,
    enrage::core::DiagnosticCode code) {
    for (const auto& diagnostic : runtime.diagnostics()) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

void write_package(
    const std::filesystem::path& source,
    const std::filesystem::path& package,
    const std::filesystem::path& plugin,
    std::string_view manifest,
    std::string_view content,
    bool valid_plugin = true) {
    std::filesystem::remove_all(source);
    std::filesystem::create_directories(source / "files" / "data");
    std::filesystem::create_directories(source / "bin");
    std::ofstream{source / "mod.json"} << manifest;
    std::ofstream{source / "files" / "data" / "value.txt"} << content;

    const auto target = source / "bin" / plugin.filename();
    if (valid_plugin) {
        std::filesystem::copy_file(plugin, target);
    } else {
        std::ofstream{target, std::ios::binary} << "not a dynamic library";
    }

    assert(enrage::core::Archive::create(source, package));
}

std::string manifest_for(
    const std::filesystem::path& plugin,
    std::string_view extra = {}) {
    std::string manifest = "{\"id\":\"example.reload\",\"nativePlugins\":[\"bin/";
    manifest += plugin.filename().generic_string();
    manifest += "\"]";
    if (!extra.empty()) {
        manifest += ",";
        manifest += extra;
    }
    manifest += "}";
    return manifest;
}

} // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path plugin_path = argv[1];

    const auto root = std::filesystem::temp_directory_path() / "enrage-mod-reload-test";
    const auto mods = root / "mods";
    const auto source = root / "source";
    const auto package = mods / "reload.enr";
    std::filesystem::remove_all(root);

    write_package(
        source,
        package,
        plugin_path,
        manifest_for(plugin_path),
        "old");

    enrage::core::Runtime runtime{
        std::make_unique<enrage::adapters::NullAdapter>(),
        mods};
    assert(runtime.start());
    assert(runtime.extensions_active());
    assert(runtime.plugins().plugins().size() == 1);
    assert(runtime.plugins().tick_callback_count() == 1);
    assert(runtime.overlay().read_text("data/value.txt") == "old");

    std::atomic_bool stop_status{false};
    std::atomic_bool invalid_status{false};
    std::thread status_reader([&] {
        while (!stop_status.load(std::memory_order_acquire)) {
            const auto status = enrage::core::runtime_status_json(runtime);
            if (!status.starts_with("{\"schema\":1") ||
                status.find("\"adapter\":") == std::string::npos ||
                status.find("\"mods\":[") == std::string::npos ||
                status.find("\"plugins\":[") == std::string::npos ||
                status.find("\"diagnostics\":[") == std::string::npos ||
                status.empty() || status.back() != '}') {
                invalid_status.store(true, std::memory_order_release);
                return;
            }
        }
    });

    // Plugin-level hot reload is finer-grained than a package-set reload. A
    // corrupt replacement must leave the live shadow generation and VFS state
    // untouched while recording a precise reload diagnostic.
    const auto invalid_replacement = root /
        ("invalid-hot-reload" + plugin_path.extension().string());
    std::ofstream{invalid_replacement, std::ios::binary | std::ios::trunc}
        << "not a native plugin";
    const auto live_shadow = runtime.plugins().plugins()[0].loaded_path;
    assert(!runtime.reload_plugin("dev.enrage.test", invalid_replacement));
    assert(has_diagnostic(runtime, enrage::core::DiagnosticCode::plugin_reload_failed));
    assert(runtime.plugins().plugins().size() == 1);
    assert(runtime.plugins().plugins()[0].loaded_path == live_shadow);
    assert(runtime.plugins().tick_callback_count() == 1);
    assert(runtime.overlay().read_text("data/value.txt") == "old");
    runtime.tick(1.0 / 60.0);
    std::filesystem::remove(invalid_replacement);

    // A valid reload stages a fresh shadow library and callback set before the
    // old generation is released. Exactly one generation remains live.
    const auto old_shadow = runtime.plugins().plugins()[0].loaded_path;
    assert(runtime.reload_plugin("dev.enrage.test"));
    assert(runtime.plugins().plugins().size() == 1);
    assert(runtime.plugins().tick_callback_count() == 1);
    assert(runtime.plugins().plugins()[0].loaded_path != old_shadow);
    assert(std::filesystem::exists(runtime.plugins().plugins()[0].loaded_path));
    assert(!std::filesystem::exists(old_shadow));
    assert(runtime.overlay().read_text("data/value.txt") == "old");
    runtime.tick(1.0 / 60.0);

    const auto original_backing = runtime.loaded_mods().at(0).archive_path;
    assert(std::filesystem::is_regular_file(original_backing));
    assert(original_backing != package);

    // Replacing the source archive with a malformed manifest must not affect
    // the already-mounted stable container or running plugin.
    write_package(
        source,
        package,
        plugin_path,
        "{not-json",
        "corrupt");
    assert(!runtime.reload_mods());
    assert(runtime.extensions_active());
    assert(runtime.plugins().plugins().size() == 1);
    assert(runtime.overlay().read_text("data/value.txt") == "old");
    assert(runtime.loaded_mods().at(0).archive_path == original_backing);
    assert(has_diagnostic(runtime, enrage::core::DiagnosticCode::reload_rejected));

    // A valid package whose graph no longer resolves is also rejected before
    // the live extension set is touched.
    write_package(
        source,
        package,
        plugin_path,
        manifest_for(plugin_path, "\"dependencies\":[\"missing.mod\"]"),
        "bad-graph");
    assert(!runtime.reload_mods());
    assert(runtime.extensions_active());
    assert(runtime.overlay().read_text("data/value.txt") == "old");

    // A package may validate structurally but still contain a native module
    // that the OS refuses to load. Activation failure must roll back too.
    write_package(
        source,
        package,
        plugin_path,
        manifest_for(plugin_path),
        "bad-plugin",
        false);
    assert(!runtime.reload_mods());
    assert(runtime.extensions_active());
    assert(runtime.plugins().plugins().size() == 1);
    assert(runtime.overlay().read_text("data/value.txt") == "old");

    // Finally swap in a valid package. Its new stable container becomes the
    // live backing store and the extension is loaded from the new generation.
    write_package(
        source,
        package,
        plugin_path,
        manifest_for(plugin_path),
        "new");
    assert(runtime.reload_mods());
    assert(runtime.extensions_active());
    assert(runtime.plugins().plugins().size() == 1);
    assert(runtime.overlay().read_text("data/value.txt") == "new");
    assert(runtime.loaded_mods().at(0).archive_path != original_backing);
    assert(std::filesystem::is_regular_file(runtime.loaded_mods().at(0).archive_path));

    stop_status.store(true, std::memory_order_release);
    status_reader.join();
    assert(!invalid_status.load(std::memory_order_acquire));

    const auto final_snapshot = runtime.snapshot();
    assert(final_snapshot.running);
    assert(final_snapshot.extensions_active);
    assert(final_snapshot.loaded_mods.size() == 1);
    assert(final_snapshot.plugins.size() == 1);
    assert(final_snapshot.plugins[0].id == "dev.enrage.test");

    runtime.stop();
    std::filesystem::remove_all(root);
    return 0;
}
