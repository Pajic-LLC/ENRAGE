#include "adapters/null_adapter.hpp"
#include "core/archive.hpp"
#include "core/manifest.hpp"
#include "core/mod_graph.hpp"
#include "core/mod_registry.hpp"
#include "core/overlay.hpp"
#include "core/package.hpp"
#include "core/plugin_host.hpp"
#include "core/runtime.hpp"
#include "platform/signature/pattern.hpp"
#include "platform/signature/resolver.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

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

void symbol_resolver_requires_unique_matches() {
    const std::array bytes{
        std::byte{0x90}, std::byte{0x48}, std::byte{0x8B}, std::byte{0x11}, std::byte{0x89},
        std::byte{0x90}, std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0x90},
    };

    auto primary = enrage::platform::signature::Pattern::parse("48 8B ?? 89");
    auto fallback = enrage::platform::signature::Pattern::parse("AA BB CC");
    assert(primary && fallback);

    enrage::platform::signature::Resolver resolver;
    assert(resolver.add({
        "ExampleSymbol",
        {
            {std::move(*primary), 0},
            {std::move(*fallback), -5},
        },
    }));

    const auto resolution = resolver.resolve("ExampleSymbol", bytes);
    assert(resolution.state == enrage::platform::signature::ResolutionState::unique);
    assert(resolution.offset == 1);

    auto repeated = enrage::platform::signature::Pattern::parse("90");
    assert(repeated);
    assert(resolver.add({"Ambiguous", {{std::move(*repeated), 0}}}));
    const auto ambiguous = resolver.resolve("Ambiguous", bytes);
    assert(ambiguous.state == enrage::platform::signature::ResolutionState::ambiguous);
    assert(ambiguous.match_count > 1);

    const auto unknown = resolver.resolve("Missing", bytes);
    assert(unknown.state == enrage::platform::signature::ResolutionState::unknown_symbol);
}

void manifest_parser_reads_package_metadata() {
    constexpr auto json = R"json({
        "id": "dev.enrage.example",
        "name": "Example",
        "version": "1.2.3",
        "priority": 12,
        "dependencies": ["dev.enrage.core"],
        "loadAfter": ["dev.enrage.visuals"],
        "conflicts": ["dev.enrage.other"],
        "contentRoot": "files",
        "nativePlugins": ["bin/example.dll"],
        "futureField": {"safe": true}
    })json";

    const auto parsed = enrage::core::parse_manifest(json);
    assert(parsed.ok());
    assert(parsed.manifest->id == "dev.enrage.example");
    assert(parsed.manifest->version == "1.2.3");
    assert(parsed.manifest->priority == 12);
    assert(parsed.manifest->dependencies.size() == 1);
    assert(parsed.manifest->native_plugins.size() == 1);

    const auto invalid = enrage::core::parse_manifest(R"({"name":"missing id"})");
    assert(!invalid.ok());
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
    assert(*resolved == std::filesystem::weakly_canonical(high / "data" / "value.bin"));
    assert(overlay.read_text("data/value.bin") == "high");
    assert(!overlay.resolve("../outside.bin").has_value());
    assert(!overlay.read("../outside.bin").has_value());

    assert(overlay.unmount("high"));
    const auto fallback = overlay.resolve("data/value.bin");
    assert(fallback.has_value());
    assert(*fallback == std::filesystem::weakly_canonical(low / "data" / "value.bin"));
    assert(overlay.read_text("data/value.bin") == "low");

    std::filesystem::remove_all(root);
}

void archive_round_trip() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-archive-test";
    const auto source = root / "source";
    const auto package = root / "example.enr";
    const auto extracted = root / "extracted";
    const auto selected = root / "selected" / "value.txt";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(source / "files" / "data");
    std::ofstream{source / "mod.json"} << R"({"id":"example.archive"})";
    std::ofstream{source / "files" / "data" / "value.txt"} << "hello";

    assert(enrage::core::Archive::create(source, package));
    const auto archive = enrage::core::Archive::open(package);
    assert(archive.has_value());
    assert(archive->entries().size() == 2);
    assert(archive->contains("mod.json"));
    assert(archive->contains("files/data/value.txt"));
    assert(archive->contains_prefix("files"));
    assert(!archive->contains_prefix("missing"));
    assert(archive->read_text("files/data/value.txt") == "hello");
    assert(archive->extract("files/data/value.txt", selected));
    assert(read_file(selected) == "hello");
    assert(archive->extract_all(extracted));
    assert(read_file(extracted / "files" / "data" / "value.txt") == "hello");

    std::filesystem::remove_all(root);
}

void package_loader_keeps_archive_content_virtual() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-package-test";
    const auto mods = root / "mods";
    const auto loose = mods / "loose";
    const auto source = root / "packed-source";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(loose / "files");
    std::filesystem::create_directories(source / "files");
    std::ofstream{loose / "mod.json"} << R"({"id":"example.loose","priority":1})";
    std::ofstream{loose / "files" / "loose.txt"} << "loose";
    std::ofstream{source / "mod.json"} << R"({"id":"example.packed","priority":2})";
    std::ofstream{source / "files" / "packed.txt"} << "packed";
    assert(enrage::core::Archive::create(source, mods / "packed.enr"));

    enrage::core::ModRegistry registry{mods};
    registry.discover();
    auto result = enrage::core::PackageLoader::load(registry, root / ".enrage");
    assert(result.mods.size() == 2);
    assert(result.diagnostics.empty());
    assert(result.mods[0].manifest.id == "example.loose");
    assert(result.mods[1].manifest.id == "example.packed");
    assert(result.mods[1].kind == enrage::core::PackageKind::archive);
    assert(result.mods[1].content_root == std::filesystem::path{"files"});
    assert(!std::filesystem::is_regular_file(result.mods[1].root / "files" / "packed.txt"));

    enrage::core::Overlay overlay;
    assert(overlay.mount_archive(
        result.mods[1].manifest.id,
        result.mods[1].source,
        result.mods[1].content_root,
        1));
    assert(overlay.read_text("packed.txt") == "packed");
    assert(!overlay.resolve("packed.txt").has_value());

    std::filesystem::remove_all(root);
}

void plugin_host_loads_c_abi_plugin(const std::filesystem::path& plugin_path) {
    enrage::core::PluginHost host;
    host.set_capabilities(ENRAGE_CAPABILITY_TICK);

    assert(host.load(plugin_path));
    assert(host.plugins().size() == 1);
    assert(host.plugins()[0].id == "dev.enrage.test");
    assert(host.tick_callback_count() == 1);

    assert(host.tick(1.0 / 60.0));
    host.unload_all();
    assert(host.plugins().empty());
    assert(host.tick_callback_count() == 0);
}

void runtime_loads_drop_in_mod(const std::filesystem::path& plugin_path) {
    const auto root = std::filesystem::temp_directory_path() / "enrage-runtime-package-test";
    const auto mods = root / "mods";
    const auto mod = mods / "example";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(mod / "files" / "data");
    std::filesystem::create_directories(mod / "bin");

    const auto plugin_name = plugin_path.filename().string();
    std::filesystem::copy_file(plugin_path, mod / "bin" / plugin_name);
    std::ofstream{mod / "files" / "data" / "value.txt"} << "override";
    std::ofstream manifest{mod / "mod.json"};
    manifest << "{\"id\":\"example.runtime\",\"nativePlugins\":[\"bin/" << plugin_name << "\"]}";
    manifest.close();

    enrage::core::Runtime runtime{
        std::make_unique<enrage::adapters::NullAdapter>(), mods};

    assert(runtime.start());
    assert(runtime.running());
    assert(runtime.loaded_mods().size() == 1);
    assert(runtime.diagnostics().empty());
    assert(runtime.plugins().plugins().size() == 1);
    const auto resolved = runtime.overlay().resolve("data/value.txt");
    assert(resolved.has_value());
    assert(read_file(*resolved) == "override");
    assert(runtime.overlay().read_text("data/value.txt") == "override");
    runtime.tick(1.0 / 60.0);
    runtime.stop();
    assert(!runtime.running());

    std::filesystem::remove_all(root);
}

void runtime_reads_packaged_content_without_extraction() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-runtime-archive-test";
    const auto mods = root / "mods";
    const auto source = root / "source";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(source / "files" / "data");
    std::ofstream{source / "mod.json"} << R"({"id":"example.runtime.archive"})";
    std::ofstream{source / "files" / "data" / "value.txt"} << "virtual";
    assert(enrage::core::Archive::create(source, mods / "example.enr"));

    enrage::core::Runtime runtime{
        std::make_unique<enrage::adapters::NullAdapter>(), mods};
    assert(runtime.start());
    assert(runtime.loaded_mods().size() == 1);
    assert(runtime.loaded_mods()[0].kind == enrage::core::PackageKind::archive);
    assert(runtime.overlay().read_text("data/value.txt") == "virtual");
    assert(!runtime.overlay().resolve("data/value.txt").has_value());
    assert(!std::filesystem::exists(runtime.loaded_mods()[0].root / "files" / "data" / "value.txt"));
    runtime.stop();

    std::filesystem::remove_all(root);
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
    const std::filesystem::path plugin_path = argv[1];

    pattern_scanner_supports_wildcards();
    symbol_resolver_requires_unique_matches();
    manifest_parser_reads_package_metadata();
    registry_discovers_directories_and_packages();
    mod_graph_resolves_dependencies_and_order_hints();
    mod_graph_reports_invalid_sets();
    overlay_prefers_highest_priority_and_rejects_traversal();
    archive_round_trip();
    package_loader_keeps_archive_content_virtual();
    plugin_host_loads_c_abi_plugin(plugin_path);
    runtime_loads_drop_in_mod(plugin_path);
    runtime_reads_packaged_content_without_extraction();
    runtime_lifecycle_is_idempotent();
    return 0;
}
