#include "adapters/capability_map.hpp"
#include "adapters/lab_adapter.hpp"
#include "core/runtime.hpp"
#include "enrage/sdk.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

const enrage::adapters::AnchorHealth* find_anchor(
    const enrage::adapters::AdapterHealthSnapshot& snapshot,
    std::string_view name) {
    const auto found = std::ranges::find(snapshot.anchors, name, &enrage::adapters::AnchorHealth::name);
    return found == snapshot.anchors.end() ? nullptr : &*found;
}

const enrage::adapters::CapabilityHealth* find_capability(
    const enrage::adapters::AdapterHealthSnapshot& snapshot,
    std::uint64_t capability) {
    const auto found = std::ranges::find(
        snapshot.capabilities,
        capability,
        &enrage::adapters::CapabilityHealth::capability);
    return found == snapshot.capabilities.end() ? nullptr : &*found;
}

std::size_t diagnostic_count(
    const enrage::core::Runtime& runtime,
    enrage::core::DiagnosticCode code) {
    return static_cast<std::size_t>(std::ranges::count(runtime.diagnostics(), code, &enrage::core::Diagnostic::code));
}

bool diagnostic_contains(
    const enrage::core::Runtime& runtime,
    enrage::core::DiagnosticCode code,
    std::string_view text) {
    return std::ranges::any_of(runtime.diagnostics(), [&](const enrage::core::Diagnostic& diagnostic) {
        return diagnostic.code == code && diagnostic.message.find(text) != std::string::npos;
    });
}

void anchor_requirements_mask_only_affected_capabilities() {
    enrage::adapters::CapabilityMap map;
    assert(map.define_anchor("scheduler"));
    assert(map.define_anchor("virtual-files"));
    assert(map.define_anchor("native-bridge"));
    assert(!map.define_anchor("scheduler"));

    assert(map.require(ENRAGE_CAPABILITY_TICK, {"scheduler"}));
    assert(map.require(ENRAGE_CAPABILITY_VIRTUAL_FILES, {"virtual-files"}));
    assert(map.require(ENRAGE_CAPABILITY_NATIVE_CALLS, {"native-bridge"}));
    assert(!map.require(ENRAGE_CAPABILITY_TICK, {"scheduler"}));

    constexpr auto advertised = ENRAGE_CAPABILITY_TICK |
                                ENRAGE_CAPABILITY_VIRTUAL_FILES |
                                ENRAGE_CAPABILITY_NATIVE_CALLS |
                                ENRAGE_CAPABILITY_ASSETS;
    assert(map.resolve(advertised) == advertised);

    assert(map.set_anchor_state(
        "scheduler",
        enrage::adapters::AnchorHealthState::degraded,
        "fallback signature selected"));
    assert(map.resolve(advertised) == advertised);
    auto snapshot = map.snapshot(advertised);
    const auto* scheduler = find_anchor(snapshot, "scheduler");
    const auto* tick = find_capability(snapshot, ENRAGE_CAPABILITY_TICK);
    assert(scheduler);
    assert(scheduler->state == enrage::adapters::AnchorHealthState::degraded);
    assert(scheduler->reason == "fallback signature selected");
    assert(tick);
    assert(tick->available);
    assert(tick->degraded_anchors == std::vector<std::string>{"scheduler"});

    assert(map.set_anchor_state(
        "scheduler",
        enrage::adapters::AnchorHealthState::missing,
        "signature not found"));
    auto available = map.resolve(advertised);
    assert((available & ENRAGE_CAPABILITY_TICK) == 0);
    assert((available & ENRAGE_CAPABILITY_VIRTUAL_FILES) != 0);
    assert((available & ENRAGE_CAPABILITY_NATIVE_CALLS) != 0);
    assert((available & ENRAGE_CAPABILITY_ASSETS) != 0);

    snapshot = map.snapshot(advertised);
    tick = find_capability(snapshot, ENRAGE_CAPABILITY_TICK);
    assert(tick);
    assert(!tick->available);
    assert(tick->blocking_anchors == std::vector<std::string>{"scheduler"});

    assert(map.set_anchor("virtual-files", false));
    available = map.resolve(advertised);
    assert((available & ENRAGE_CAPABILITY_TICK) == 0);
    assert((available & ENRAGE_CAPABILITY_VIRTUAL_FILES) == 0);
    assert((available & ENRAGE_CAPABILITY_NATIVE_CALLS) != 0);

    assert(!map.set_anchor("missing", true));
}

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
    assert(runtime.capabilities() == state->capability_bits);
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

void runtime_degrades_capabilities_independently(const std::filesystem::path& plugin_path) {
    const auto root = std::filesystem::temp_directory_path() / "enrage-lab-capability-degradation-test";
    const auto mods = root / "mods";
    const auto mod = mods / "example";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(mod / "files" / "data");
    std::filesystem::create_directories(mod / "bin");

    const auto plugin_name = plugin_path.filename().string();
    std::filesystem::copy_file(plugin_path, mod / "bin" / plugin_name);
    std::ofstream{mod / "files" / "data" / "value.txt"} << "active";
    std::ofstream manifest{mod / "mod.json"};
    manifest << "{\"id\":\"example.capabilities\",\"nativePlugins\":[\"bin/"
             << plugin_name << "\"]}";
    manifest.close();

    auto state = std::make_shared<enrage::adapters::LabState>();
    state->capability_bits = ENRAGE_CAPABILITY_TICK |
                             ENRAGE_CAPABILITY_VIRTUAL_FILES |
                             ENRAGE_CAPABILITY_NATIVE_CALLS;
    state->environment = enrage::adapters::EnvironmentState::local;
    assert(state->capability_map.define_anchor("scheduler"));
    assert(state->capability_map.define_anchor("virtual-files"));
    assert(state->capability_map.define_anchor("native-bridge"));
    assert(state->capability_map.require(ENRAGE_CAPABILITY_TICK, {"scheduler"}));
    assert(state->capability_map.require(ENRAGE_CAPABILITY_VIRTUAL_FILES, {"virtual-files"}));
    assert(state->capability_map.require(ENRAGE_CAPABILITY_NATIVE_CALLS, {"native-bridge"}));

    enrage::core::Runtime runtime{
        std::make_unique<enrage::adapters::LabAdapter>(state), mods};
    assert(runtime.start());
    assert(runtime.extensions_active());
    assert(runtime.plugins().plugins().size() == 1);
    assert(runtime.plugins().tick_callback_count() == 1);
    assert(runtime.overlay().resolve("data/value.txt").has_value());
    assert(runtime.health().anchors.size() == 3);
    assert(diagnostic_count(runtime, enrage::core::DiagnosticCode::anchor_health_changed) == 0);
    runtime.tick(1.0 / 60.0);

    assert(state->capability_map.set_anchor_state(
        "scheduler",
        enrage::adapters::AnchorHealthState::degraded,
        "fallback pattern selected"));
    runtime.tick(1.0 / 60.0);
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_TICK) != 0);
    const auto* scheduler = find_anchor(runtime.health(), "scheduler");
    const auto* tick = find_capability(runtime.health(), ENRAGE_CAPABILITY_TICK);
    assert(scheduler && scheduler->state == enrage::adapters::AnchorHealthState::degraded);
    assert(tick && tick->available);
    assert(tick->degraded_anchors == std::vector<std::string>{"scheduler"});
    assert(diagnostic_contains(
        runtime,
        enrage::core::DiagnosticCode::anchor_health_changed,
        "fallback pattern selected"));
    assert(diagnostic_contains(
        runtime,
        enrage::core::DiagnosticCode::capability_health_changed,
        "available with degraded anchors: scheduler"));

    assert(state->capability_map.set_anchor_state(
        "scheduler",
        enrage::adapters::AnchorHealthState::missing,
        "scheduler signature not found"));
    runtime.tick(1.0 / 60.0);
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_TICK) == 0);
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_VIRTUAL_FILES) != 0);
    assert(runtime.plugins().plugins().size() == 1);
    assert(runtime.plugins().tick_callback_count() == 1);
    assert(runtime.overlay().resolve("data/value.txt").has_value());
    tick = find_capability(runtime.health(), ENRAGE_CAPABILITY_TICK);
    assert(tick && !tick->available);
    assert(tick->blocking_anchors == std::vector<std::string>{"scheduler"});
    assert(diagnostic_contains(
        runtime,
        enrage::core::DiagnosticCode::anchor_health_changed,
        "scheduler signature not found"));
    assert(diagnostic_contains(
        runtime,
        enrage::core::DiagnosticCode::capability_health_changed,
        "tick is suppressed by scheduler"));

    const auto diagnostics_after_missing = runtime.diagnostics().size();
    runtime.tick(1.0 / 60.0);
    assert(runtime.diagnostics().size() == diagnostics_after_missing);

    assert(state->capability_map.set_anchor_state(
        "virtual-files",
        enrage::adapters::AnchorHealthState::missing,
        "overlay hook signature not found"));
    runtime.tick(1.0 / 60.0);
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_VIRTUAL_FILES) == 0);
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_NATIVE_CALLS) != 0);
    assert(runtime.plugins().plugins().size() == 1);
    assert(!runtime.overlay().resolve("data/value.txt").has_value());

    assert(state->capability_map.set_anchor_state(
        "virtual-files",
        enrage::adapters::AnchorHealthState::resolved,
        "replacement signature verified"));
    runtime.tick(1.0 / 60.0);
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_VIRTUAL_FILES) != 0);
    assert(runtime.plugins().plugins().size() == 1);
    assert(runtime.overlay().resolve("data/value.txt").has_value());

    assert(state->capability_map.set_anchor_state(
        "scheduler",
        enrage::adapters::AnchorHealthState::resolved,
        "replacement signature verified"));
    runtime.tick(1.0 / 60.0);
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_TICK) != 0);
    assert(runtime.plugins().plugins().size() == 1);
    assert(diagnostic_contains(
        runtime,
        enrage::core::DiagnosticCode::capability_health_changed,
        "tick is available"));

    assert(state->capability_map.set_anchor_state(
        "native-bridge",
        enrage::adapters::AnchorHealthState::unresolved,
        "new build has not been researched"));
    runtime.tick(1.0 / 60.0);
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_NATIVE_CALLS) == 0);
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_TICK) != 0);
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_VIRTUAL_FILES) != 0);
    assert(runtime.plugins().plugins().size() == 1);
    assert(runtime.overlay().resolve("data/value.txt").has_value());
    assert(diagnostic_contains(
        runtime,
        enrage::core::DiagnosticCode::anchor_health_changed,
        "new build has not been researched"));

    runtime.stop();
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

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path plugin_path = argv[1];

    anchor_requirements_mask_only_affected_capabilities();
    runtime_transitions_are_deterministic();
    runtime_degrades_capabilities_independently(plugin_path);
    failure_modes_are_controllable();
    return 0;
}
