#include "adapters/lab_adapter.hpp"
#include "core/runtime.hpp"
#include "enrage/sdk.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace {

void runtime_transitions_are_deterministic() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-lab-adapter-test";
    const auto mods = root / "mods";
    const auto mod = mods / "example";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(mod / "files" / "data");
    std::ofstream{mod / "mod.json"} << R"({"id":"example.lab"})";
    std::ofstream{mod / "files" / "data" / "value.txt"} << "active";

    auto state = std::make_shared<enrage::adapters::LabState>();
    state->adapter_name = "Fixture Lab";
    state->build = {"fixture.100", 100};
    state->capability_bits = ENRAGE_CAPABILITY_VIRTUAL_FILES | ENRAGE_CAPABILITY_TICK;
    state->environment = enrage::adapters::EnvironmentState::unknown;

    enrage::core::Runtime runtime{
        std::make_unique<enrage::adapters::LabAdapter>(state), mods};

    assert(runtime.start());
    assert(runtime.adapter().name() == "Fixture Lab");
    assert(runtime.adapter().build().id == "fixture.100");
    assert(runtime.adapter().build().fingerprint == 100);
    assert(!runtime.extensions_active());
    assert(state->probe_count == 1);
    assert(state->initialize_count == 1);
    assert(state->initialized);

    state->environment = enrage::adapters::EnvironmentState::local;
    runtime.tick(0.0);
    assert(runtime.extensions_active());
    assert(runtime.overlay().resolve("data/value.txt").has_value());

    state->environment = enrage::adapters::EnvironmentState::restricted;
    runtime.tick(0.0);
    assert(!runtime.extensions_active());
    assert(!runtime.overlay().resolve("data/value.txt").has_value());

    state->build = {"fixture.101", 101};
    state->environment = enrage::adapters::EnvironmentState::local;
    runtime.tick(0.0);
    assert(runtime.extensions_active());
    assert(runtime.adapter().build().id == "fixture.101");
    assert(runtime.adapter().build().fingerprint == 101);

    runtime.stop();
    assert(!state->initialized);
    assert(state->shutdown_count == 1);
    const std::vector<std::string> expected{"probe", "initialize", "shutdown"};
    assert(state->events == expected);

    std::filesystem::remove_all(root);
}

void failure_modes_are_controllable() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-lab-adapter-failure-test";
    const auto mods = root / "mods";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(mods);

    {
        auto state = std::make_shared<enrage::adapters::LabState>();
        state->probe_result = false;
        enrage::core::Runtime runtime{
            std::make_unique<enrage::adapters::LabAdapter>(state), mods};
        assert(!runtime.start());
        assert(state->probe_count == 1);
        assert(state->initialize_count == 0);
    }

    {
        auto state = std::make_shared<enrage::adapters::LabState>();
        state->initialize_result = false;
        enrage::core::Runtime runtime{
            std::make_unique<enrage::adapters::LabAdapter>(state), mods};
        assert(!runtime.start());
        assert(state->probe_count == 1);
        assert(state->initialize_count == 1);
        assert(!state->initialized);
        assert(state->shutdown_count == 0);
    }

    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    runtime_transitions_are_deterministic();
    failure_modes_are_controllable();
    return 0;
}
