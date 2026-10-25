#include "adapters/null_adapter.hpp"
#include "core/runtime.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

bool has_reload_failure(const enrage::core::Runtime& runtime) {
    for (const auto& diagnostic : runtime.diagnostics()) {
        if (diagnostic.code == enrage::core::DiagnosticCode::plugin_reload_failed &&
            diagnostic.mod_id == "example.reload") {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path plugin_source = argv[1];

    const auto root = std::filesystem::temp_directory_path() / "enrage-runtime-plugin-reload-test";
    const auto mods = root / "mods";
    const auto mod = mods / "example";
    const auto bin = mod / "bin";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(bin);
    std::filesystem::create_directories(mod / "files" / "data");

    const auto plugin_path = bin / plugin_source.filename();
    std::filesystem::copy_file(plugin_source, plugin_path);
    std::ofstream{mod / "files" / "data" / "value.txt"} << "still-mounted";
    std::ofstream manifest{mod / "mod.json"};
    manifest << "{\"id\":\"example.reload\",\"nativePlugins\":[\"bin/"
             << plugin_path.filename().string() << "\"]}";
    manifest.close();

    enrage::core::Runtime runtime{
        std::make_unique<enrage::adapters::NullAdapter>(),
        mods};
    assert(runtime.start());
    assert(runtime.extensions_active());
    assert(runtime.plugins().plugins().size() == 1);
    assert(runtime.plugins().tick_callback_count() == 1);
    assert(runtime.overlay().resolve("data/value.txt").has_value());
    assert(runtime.tick(1.0 / 60.0), true);

    const auto invalid = root / ("invalid" + plugin_path.extension().string());
    std::ofstream{invalid, std::ios::binary | std::ios::trunc} << "not a native plugin";
    const auto live_shadow = runtime.plugins().plugins()[0].loaded_path;
    assert(!runtime.reload_plugin("dev.enrage.test", invalid));
    assert(has_reload_failure(runtime));
    assert(runtime.plugins().plugins().size() == 1);
    assert(runtime.plugins().plugins()[0].loaded_path == live_shadow);
    assert(runtime.plugins().tick_callback_count() == 1);
    assert(runtime.overlay().resolve("data/value.txt").has_value());
    runtime.tick(1.0 / 60.0);

    const auto old_shadow = runtime.plugins().plugins()[0].loaded_path;
    assert(runtime.reload_plugin("dev.enrage.test"));
    assert(runtime.plugins().plugins().size() == 1);
    assert(runtime.plugins().tick_callback_count() == 1);
    assert(runtime.plugins().plugins()[0].loaded_path != old_shadow);
    assert(!std::filesystem::exists(old_shadow));
    assert(runtime.overlay().resolve("data/value.txt").has_value());
    runtime.tick(1.0 / 60.0);

    runtime.stop();
    std::filesystem::remove_all(root);
    return 0;
}
