#include "adapters/null_adapter.hpp"
#include "core/mod_graph.hpp"
#include "core/mod_registry.hpp"
#include "core/overlay.hpp"
#include "core/plugin_host.hpp"
#include "core/runtime.hpp"
#include "platform/signature/pattern.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace {

void pattern_scanner_supports_wildcards() {
    const auto pattern = enrage::platform::signature::Pattern::parse("48 8B ?? 89");
    assert(pattern.has_value());

    const std::array bytes{
        std::byte{0x90}, std::byte{0x48}, std::byte{0x8B},
        std::byte{0x12}, std::byte{0x89}, std::byte{0x90},
    };

    const auto match = pattern->find(bytes);
    assert(match.has_value());
    assert(*match == 1);
}

void registry_discovers_directories_and_packages() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-registry-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "alpha");
    std::ofstream{root / "beta.enr"} << "test";
    std::ofstream{root / "ignored.txt"} << "test";

    enrage::core::ModRegistry registry{root};
    registry.discover();

    assert(registry.candidates().size() == 2);
    assert(registry.candidates()[0].id == "alpha");
    assert(registry.candidates()[1].id == "beta");
    assert(registry.candidates()[1].packaged);

    std::filesystem::remove_all(root);
}

void mod_graph_resolves_dependencies_and_order_hints() {
    const std::vector<enrage::core::ModSpec> mods{
        {.id = "visuals", .priority = 20, .dependencies = {"core"}},
        {.id = "traffic", .priority = 5, .load_after = {"visuals"}},
        {.id = "core", .priority = 0},
    };

    const auto resolution = enrage::core::ModGraph::resolve(mods);
    assert(resolution.ok());
    assert((resolution.order == std::vector<std::string>{"core", "visuals", "traffic"}));
}

void mod_graph_reports_invalid_sets() {
    const std::vector<enrage::core::ModSpec> missing{
        {.id = "alpha", .dependencies = {"missing"}},
    };
    const auto missing_result = enrage::core::ModGraph::resolve(missing);
    assert(!missing_result.ok());
    assert(missing_result.issues[0].kind == enrage::core::ModIssueKind::missing_dependency);

    const std::vector<enrage::core::ModSpec> conflicting{
        {.id = "alpha", .conflicts = {"beta"}},
        {.id = "beta", .conflicts = {"alpha"}},
    };
    const auto conflict_result = enrage::core::ModGraph::resolve(conflicting);
    assert(!conflict_result.ok());
    assert(conflict_result.issues.size() == 1);
    assert(conflict_result.issues[0].kind == enrage::core::ModIssueKind::conflict);

    const std::vector<enrage::core::ModSpec> cyclic{
        {.id = "alpha", .load_after = {"beta"}},
        {.id = "beta", .load_after = {"alpha"}},
    };
    const auto cycle_result = enrage::core::ModGraph::resolve(cyclic);
    assert(!cycle_result.ok());
    assert(cycle_result.order.empty());
}

void overlay_prefers_highest_priority_and_rejects_traversal() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-overlay-test";
    const auto low = root / "low";
    const auto high = root / "high";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(low / "data");
    std::filesystem::create_directories(high / "data");
    std::ofstream{low / "data" / "value.bin"} << "low";
    std::ofstream{high / "data" / "value.bin"} << "high";

    enrage::core::Overlay overlay;
    assert(overlay.mount({"low", low, 10}));
    assert(overlay.mount({"high", high, 20}));
    assert(!overlay.mount({"high", high, 30}));

    const auto resolved = overlay.resolve("data/value.bin");
    assert(resolved.has_value());
    assert(*resolved == high / "data" / "value.bin");
    assert(!overlay.resolve("../outside.bin").has_value());

    assert(overlay.unmount("high"));
    const auto fallback = overlay.resolve("data/value.bin");
    assert(fallback.has_value());
    assert(*fallback == low / "data" / "value.bin");

    std::filesystem::remove_all(root);
}

void plugin_host_loads_c_abi_plugin(const std::filesystem::path& plugin_path) {
    enrage::core::PluginHost host;
    host.set_capabilities(ENRAGE_CAPABILITY_TICK);

    assert(host.load(plugin_path));
    assert(host.plugins().size() == 1);
    assert(host.plugins()[0].id == "dev.enrage.test");
    assert(host.tick_callback_count() == 1);

    host.tick(1.0 / 60.0);
    host.unload_all();
    assert(host.plugins().empty());
    assert(host.tick_callback_count() == 0);
}

void runtime_lifecycle_is_idempotent() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-runtime-test";
    std::filesystem::remove_all(root);

    enrage::core::Runtime runtime{
        std::make_unique<enrage::adapters::NullAdapter>(), root};

    assert(runtime.start());
    assert(runtime.running());
    assert(!runtime.start());

    runtime.stop();
    assert(!runtime.running());
    runtime.stop();

    std::filesystem::remove_all(root);
}

} // namespace

int main(int argc, char** argv) {
    assert(argc == 2);

    pattern_scanner_supports_wildcards();
    registry_discovers_directories_and_packages();
    mod_graph_resolves_dependencies_and_order_hints();
    mod_graph_reports_invalid_sets();
    overlay_prefers_highest_priority_and_rejects_traversal();
    plugin_host_loads_c_abi_plugin(argv[1]);
    runtime_lifecycle_is_idempotent();
    return 0;
}
