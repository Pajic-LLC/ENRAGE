#include "core/plugin_host.hpp"

#include <cassert>
#include <filesystem>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path plugin_path = argv[1];

    enrage::core::PluginHost host;
    host.set_capabilities(ENRAGE_CAPABILITY_TICK);

    std::vector<std::string> events;
    host.set_execution_observer(
        [&](std::string_view mod_id, std::string_view plugin_id, std::string_view callback) {
            events.push_back(
                "begin:" + std::string{mod_id} + ":" + std::string{plugin_id} + ":" + std::string{callback});
            return true;
        },
        [&]() {
            events.push_back("end");
            return true;
        });

    assert(host.load(plugin_path, "example.owner"));
    assert(host.plugins().size() == 1);
    assert(host.plugins()[0].owner_mod_id == "example.owner");
    assert(host.tick_callback_count() == 1);
    assert(host.tick(1.0 / 60.0));

    assert(!host.unload("missing.plugin"));
    assert(host.unload("dev.enrage.test"));
    assert(host.plugins().empty());
    assert(host.tick_callback_count() == 0);
    assert(host.tick(1.0 / 60.0));

    // Loading again proves the dynamic library and its owned registrations were
    // fully released by the individual unload path.
    assert(host.load(plugin_path, "example.owner"));
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
        "begin:example.owner:dev.enrage.test:unload",
        "end",
        "begin:example.owner:dev.enrage.test:load",
        "end",
        "begin:example.owner:dev.enrage.test:unload",
        "end",
    };
    assert(events == expected);
    return 0;
}
