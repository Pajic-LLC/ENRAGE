#include "core/plugin_host.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

void checkpoint(std::string_view value) {
    std::cerr << "reload-test:" << value << std::endl;
}

} // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path plugin_path = argv[1];

    enrage::core::PluginHost host;
    host.set_capabilities(ENRAGE_CAPABILITY_TICK);

    std::vector<std::string> events;
    host.set_execution_observer(
        [&](std::string_view mod_id, std::string_view plugin_id, std::string_view callback) {
            checkpoint(callback);
            events.push_back(
                "begin:" + std::string{mod_id} + ":" + std::string{plugin_id} + ":" + std::string{callback});
            return true;
        },
        [&]() {
            checkpoint("execution-end");
            events.push_back("end");
            return true;
        });

    checkpoint("initial-load-before");
    assert(host.load(plugin_path, "example.owner"));
    checkpoint("initial-load-after");
    assert(host.plugins().size() == 1);
    assert(host.plugins()[0].owner_mod_id == "example.owner");
    assert(host.plugins()[0].path == plugin_path);
    assert(host.plugins()[0].loaded_path != plugin_path);
    assert(std::filesystem::exists(host.plugins()[0].loaded_path));
    assert(host.tick_callback_count() == 1);
    assert(host.tick(1.0 / 60.0));

    const auto invalid_path = plugin_path.parent_path() /
        ("invalid-replacement" + plugin_path.extension().string());
    std::ofstream{invalid_path, std::ios::binary | std::ios::trunc} << "not a dynamic library";
    const auto live_shadow_before_failure = host.plugins()[0].loaded_path;
    checkpoint("invalid-reload-before");
    assert(!host.reload("dev.enrage.test", invalid_path));
    checkpoint("invalid-reload-after");
    assert(host.plugins().size() == 1);
    assert(host.plugins()[0].loaded_path == live_shadow_before_failure);
    assert(std::filesystem::exists(live_shadow_before_failure));
    assert(host.tick_callback_count() == 1);
    assert(host.tick(1.0 / 60.0));
    std::filesystem::remove(invalid_path);

    const auto old_shadow = host.plugins()[0].loaded_path;
    checkpoint("valid-reload-before");
    assert(host.reload("dev.enrage.test"));
    checkpoint("valid-reload-after");
    assert(host.plugins().size() == 1);
    assert(host.plugins()[0].owner_mod_id == "example.owner");
    assert(host.plugins()[0].path == plugin_path);
    assert(host.plugins()[0].loaded_path != old_shadow);
    assert(std::filesystem::exists(host.plugins()[0].loaded_path));
    assert(!std::filesystem::exists(old_shadow));
    assert(host.tick_callback_count() == 1);
    assert(host.tick(1.0 / 60.0));

    assert(!host.unload("missing.plugin"));
    const auto current_shadow = host.plugins()[0].loaded_path;
    checkpoint("unload-before");
    assert(host.unload("dev.enrage.test"));
    checkpoint("unload-after");
    assert(host.plugins().empty());
    assert(host.tick_callback_count() == 0);
    assert(!std::filesystem::exists(current_shadow));
    assert(host.tick(1.0 / 60.0));

    checkpoint("reload-source-before");
    assert(host.load(plugin_path, "example.owner"));
    checkpoint("reload-source-after");
    assert(host.plugins().size() == 1);
    assert(host.tick_callback_count() == 1);
    assert(host.unload_mod("other.owner") == 0);
    assert(host.unload_mod("example.owner") == 1);
    assert(host.plugins().empty());
    assert(host.tick_callback_count() == 0);

    const std::vector<std::string> expected{
        "begin:example.owner:dev.enrage.test:load",
        "end",
        "begin:example.owner:dev.enrage.test:tick",
        "end",
        "begin:example.owner:dev.enrage.test:tick",
        "end",
        "begin:example.owner:dev.enrage.test:reload-load",
        "end",
        "begin:example.owner:dev.enrage.test:reload-unload",
        "end",
        "begin:example.owner:dev.enrage.test:tick",
        "end",
        "begin:example.owner:dev.enrage.test:unload",
        "end",
        "begin:example.owner:dev.enrage.test:load",
        "end",
        "begin:example.owner:dev.enrage.test:unload",
        "end",
    };
    assert(events == expected);
    checkpoint("done");
    return 0;
}
