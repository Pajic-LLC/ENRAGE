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
    host.unload_all();

    const std::vector<std::string> expected{
        "begin:example.owner:dev.enrage.test:load",
        "end",
        "begin:example.owner:dev.enrage.test:tick",
        "end",
        "begin:example.owner:dev.enrage.test:unload",
        "end",
    };
    assert(events == expected);
    return 0;
}
