#include "adapters/lab_adapter.hpp"
#include "core/runtime.hpp"
#include "core/status.hpp"
#include "enrage/sdk.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path plugin_path = argv[1];

    const auto root = std::filesystem::temp_directory_path() / "enrage-status-json-test";
    const auto mods = root / "mods";
    const auto mod = mods / "example";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(mod / "files");
    std::filesystem::create_directories(mod / "bin");

    const auto plugin_name = plugin_path.filename();
    std::filesystem::copy_file(plugin_path, mod / "bin" / plugin_name);
    std::ofstream{mod / "files" / "value.txt"} << "status";
    std::ofstream manifest{mod / "mod.json"};
    manifest << "{\"id\":\"example.status\",\"name\":\"Status Fixture\","
             << "\"version\":\"2.3.4\",\"nativePlugins\":[\"bin/"
             << plugin_name.generic_string() << "\"]}";
    manifest.close();

    auto state = std::make_shared<enrage::adapters::LabState>();
    state->adapter_name = "Status Lab";
    state->environment = enrage::adapters::EnvironmentState::local;
    state->capability_bits = ENRAGE_CAPABILITY_TICK | ENRAGE_CAPABILITY_VIRTUAL_FILES;

    enrage::platform::FileFingerprint fingerprint;
    fingerprint.sha256[0] = 0xab;
    fingerprint.sha256[31] = 0xcd;
    fingerprint.size = 123456;
    state->build = {"fixture.status", fingerprint};

    assert(state->capability_map.define_anchor("scheduler"));
    assert(state->capability_map.require(ENRAGE_CAPABILITY_TICK, {"scheduler"}));
    assert(state->capability_map.set_anchor_state(
        "scheduler",
        enrage::adapters::AnchorHealthState::degraded,
        "fallback candidate active"));

    enrage::core::Runtime runtime{
        std::make_unique<enrage::adapters::LabAdapter>(state),
        mods};
    assert(runtime.start());
    assert(runtime.extensions_active());
    assert(runtime.plugins().plugins().size() == 1);

    const auto json = enrage::core::runtime_status_json(runtime);
    assert(json.starts_with('{') && json.ends_with('}'));
    assert(json.find("\"schema\":1") != std::string::npos);
    assert(json.find("\"adapter\":\"Status Lab\"") != std::string::npos);
    assert(json.find("\"environment\":\"local\"") != std::string::npos);
    assert(json.find("\"id\":\"fixture.status\"") != std::string::npos);
    assert(json.find("\"sha256\":\"ab") != std::string::npos);
    assert(json.find("cd\"") != std::string::npos);
    assert(json.find("\"size\":123456") != std::string::npos);
    assert(json.find("\"name\":\"scheduler\",\"state\":\"degraded\"") != std::string::npos);
    assert(json.find("fallback candidate active") != std::string::npos);
    assert(json.find("\"id\":\"example.status\"") != std::string::npos);
    assert(json.find("\"version\":\"2.3.4\"") != std::string::npos);
    assert(json.find("\"id\":\"dev.enrage.test\"") != std::string::npos);
    assert(json.find("\"ownerMod\":\"example.status\"") != std::string::npos);
    assert(json.find("anchor_health_changed") != std::string::npos);

    // Default status output deliberately excludes local filesystem paths.
    assert(json.find(root.generic_string()) == std::string::npos);
    assert(json.find(plugin_path.generic_string()) == std::string::npos);

    runtime.stop();
    std::filesystem::remove_all(root);
    return 0;
}
