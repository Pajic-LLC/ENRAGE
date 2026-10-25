#include "adapters/lab_adapter.hpp"
#include "adapters/signature_health.hpp"
#include "core/runtime.hpp"
#include "enrage/sdk.h"
#include "platform/pe_image.hpp"
#include "platform/scan_image.hpp"
#include "platform/signature/pattern.hpp"
#include "platform/signature/resolver.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class SchedulerFixture {
    primary,
    fallback,
    ambiguous,
    missing,
};

void put16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffu);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xffu);
}

void put32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (i * 8)) & 0xffu);
    }
}

void put64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (i * 8)) & 0xffu);
    }
}

void put_name(std::vector<std::byte>& bytes, std::size_t offset, const char* name) {
    for (std::size_t i = 0; name[i] != '\0' && i < 8; ++i) {
        bytes[offset + i] = static_cast<std::byte>(name[i]);
    }
}

void put_pattern(std::vector<std::byte>& bytes, std::size_t offset, std::initializer_list<unsigned> values) {
    for (const auto value : values) bytes[offset++] = static_cast<std::byte>(value);
}

std::vector<std::byte> image_fixture(SchedulerFixture scheduler) {
    std::vector<std::byte> bytes(0x500);
    bytes[0] = std::byte{'M'};
    bytes[1] = std::byte{'Z'};
    put32(bytes, 0x3c, 0x80);

    const std::size_t pe = 0x80;
    bytes[pe] = std::byte{'P'};
    bytes[pe + 1] = std::byte{'E'};
    const auto coff = pe + 4;
    put16(bytes, coff, 0x8664);
    put16(bytes, coff + 2, 1);
    put32(bytes, coff + 4, 0x12345678);
    put16(bytes, coff + 16, 0xf0);

    const auto optional = coff + 20;
    put16(bytes, optional, 0x20b);
    put32(bytes, optional + 16, 0x1000);
    put64(bytes, optional + 24, 0x140000000ull);
    put32(bytes, optional + 56, 0x3000);

    const auto text = optional + 0xf0;
    put_name(bytes, text, ".text");
    put32(bytes, text + 8, 0x200);
    put32(bytes, text + 12, 0x1000);
    put32(bytes, text + 16, 0x200);
    put32(bytes, text + 20, 0x200);
    put32(bytes, text + 36, 0x60000020);

    switch (scheduler) {
        case SchedulerFixture::primary:
            put_pattern(bytes, 0x210, {0xAA, 0xBB, 0xCC, 0xDD});
            break;
        case SchedulerFixture::fallback:
            put_pattern(bytes, 0x210, {0x11, 0x22, 0x33, 0x44});
            break;
        case SchedulerFixture::ambiguous:
            put_pattern(bytes, 0x210, {0xAA, 0xBB, 0xCC, 0xDD});
            put_pattern(bytes, 0x250, {0xAA, 0xBB, 0xCC, 0xDD});
            break;
        case SchedulerFixture::missing:
            break;
    }

    put_pattern(bytes, 0x280, {0x55, 0x66, 0x77, 0x88});
    put_pattern(bytes, 0x2a0, {0x90, 0x91, 0x92, 0x93});
    return bytes;
}

enrage::platform::signature::Resolver make_resolver() {
    using enrage::platform::signature::Pattern;
    enrage::platform::signature::Resolver resolver;

    auto scheduler_primary = Pattern::parse("AA BB CC DD");
    auto scheduler_fallback = Pattern::parse("11 22 33 44");
    auto virtual_files = Pattern::parse("55 66 77 88");
    auto native_bridge = Pattern::parse("90 91 92 93");
    assert(scheduler_primary && scheduler_fallback && virtual_files && native_bridge);

    assert(resolver.add({
        "SchedulerSymbol",
        {
            {std::move(*scheduler_primary), 0},
            {std::move(*scheduler_fallback), 0},
        },
        ".text",
    }));
    assert(resolver.add({"VirtualFilesSymbol", {{std::move(*virtual_files), 0}}, ".text"}));
    assert(resolver.add({"NativeBridgeSymbol", {{std::move(*native_bridge), 0}}, ".text"}));
    return resolver;
}

enrage::adapters::SignatureHealthResolver make_health_resolver() {
    enrage::adapters::SignatureHealthResolver health;
    assert(health.bind("scheduler", "SchedulerSymbol"));
    assert(health.bind("virtual-files", "VirtualFilesSymbol"));
    assert(health.bind("native-bridge", "NativeBridgeSymbol"));
    assert(health.bind("assets", "AssetsSymbol"));
    assert(!health.bind("scheduler", "DuplicateSymbol"));
    return health;
}

void configure_capabilities(enrage::adapters::CapabilityMap& map) {
    using enrage::adapters::AnchorHealthState;
    assert(map.define_anchor("scheduler", AnchorHealthState::unresolved));
    assert(map.define_anchor("virtual-files", AnchorHealthState::unresolved));
    assert(map.define_anchor("native-bridge", AnchorHealthState::unresolved));
    assert(map.define_anchor("assets", AnchorHealthState::unresolved));
    assert(map.require(ENRAGE_CAPABILITY_TICK, {"scheduler"}));
    assert(map.require(ENRAGE_CAPABILITY_VIRTUAL_FILES, {"virtual-files"}));
    assert(map.require(ENRAGE_CAPABILITY_NATIVE_CALLS, {"native-bridge"}));
    assert(map.require(ENRAGE_CAPABILITY_ASSETS, {"assets"}));
}

const enrage::adapters::AnchorHealth* find_anchor(
    const enrage::adapters::AdapterHealthSnapshot& snapshot,
    std::string_view name) {
    for (const auto& anchor : snapshot.anchors) {
        if (anchor.name == name) return &anchor;
    }
    return nullptr;
}

bool has_message(const std::vector<enrage::core::Diagnostic>& diagnostics, std::string_view text) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.message.find(text) != std::string::npos) return true;
    }
    return false;
}

void resolver_outcomes_map_to_health() {
    using enrage::adapters::AnchorHealthState;

    auto resolver = make_resolver();
    auto health = make_health_resolver();
    enrage::adapters::CapabilityMap map;
    configure_capabilities(map);

    const auto primary = enrage::platform::ScanImage::from_file(image_fixture(SchedulerFixture::primary));
    assert(primary);
    assert(health.apply(map, resolver, *primary));
    auto snapshot = map.snapshot(
        ENRAGE_CAPABILITY_TICK |
        ENRAGE_CAPABILITY_VIRTUAL_FILES |
        ENRAGE_CAPABILITY_NATIVE_CALLS |
        ENRAGE_CAPABILITY_ASSETS);
    assert(find_anchor(snapshot, "scheduler")->state == AnchorHealthState::resolved);
    assert(find_anchor(snapshot, "assets")->state == AnchorHealthState::unresolved);

    const auto fallback = enrage::platform::ScanImage::from_file(image_fixture(SchedulerFixture::fallback));
    assert(fallback);
    assert(health.apply(map, resolver, *fallback));
    snapshot = map.snapshot(ENRAGE_CAPABILITY_TICK);
    const auto* scheduler = find_anchor(snapshot, "scheduler");
    assert(scheduler && scheduler->state == AnchorHealthState::degraded);
    assert(scheduler->reason.find("fallback candidate #2") != std::string::npos);
    assert((map.resolve(ENRAGE_CAPABILITY_TICK) & ENRAGE_CAPABILITY_TICK) != 0);

    const auto ambiguous = enrage::platform::ScanImage::from_file(image_fixture(SchedulerFixture::ambiguous));
    assert(ambiguous);
    assert(health.apply(map, resolver, *ambiguous));
    snapshot = map.snapshot(ENRAGE_CAPABILITY_TICK);
    scheduler = find_anchor(snapshot, "scheduler");
    assert(scheduler && scheduler->state == AnchorHealthState::missing);
    assert(scheduler->reason.find("ambiguous (2 matches)") != std::string::npos);
    assert((map.resolve(ENRAGE_CAPABILITY_TICK) & ENRAGE_CAPABILITY_TICK) == 0);

    const auto missing = enrage::platform::ScanImage::from_file(image_fixture(SchedulerFixture::missing));
    assert(missing);
    assert(health.apply(map, resolver, *missing));
    snapshot = map.snapshot(ENRAGE_CAPABILITY_TICK);
    scheduler = find_anchor(snapshot, "scheduler");
    assert(scheduler && scheduler->state == AnchorHealthState::missing);
    assert(scheduler->reason.find("signature not found") != std::string::npos);
}

void runtime_reacts_to_signature_health(const std::filesystem::path& plugin_path) {
    const auto root = std::filesystem::temp_directory_path() / "enrage-signature-health-test";
    const auto mods = root / "mods";
    const auto mod = mods / "example";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(mod / "files" / "data");
    std::filesystem::create_directories(mod / "bin");

    const auto plugin_name = plugin_path.filename().string();
    std::filesystem::copy_file(plugin_path, mod / "bin" / plugin_name);
    std::ofstream{mod / "files" / "data" / "value.txt"} << "active";
    std::ofstream manifest{mod / "mod.json"};
    manifest << "{\"id\":\"example.signature-health\",\"nativePlugins\":[\"bin/"
             << plugin_name << "\"]}";
    manifest.close();

    auto resolver = make_resolver();
    auto health = make_health_resolver();
    auto state = std::make_shared<enrage::adapters::LabState>();
    state->environment = enrage::adapters::EnvironmentState::local;
    state->capability_bits = ENRAGE_CAPABILITY_TICK |
                             ENRAGE_CAPABILITY_VIRTUAL_FILES |
                             ENRAGE_CAPABILITY_NATIVE_CALLS |
                             ENRAGE_CAPABILITY_ASSETS;
    configure_capabilities(state->capability_map);

    const auto primary = enrage::platform::ScanImage::from_file(image_fixture(SchedulerFixture::primary));
    const auto fallback = enrage::platform::ScanImage::from_file(image_fixture(SchedulerFixture::fallback));
    const auto ambiguous = enrage::platform::ScanImage::from_file(image_fixture(SchedulerFixture::ambiguous));
    const auto missing = enrage::platform::ScanImage::from_file(image_fixture(SchedulerFixture::missing));
    assert(primary && fallback && ambiguous && missing);
    assert(health.apply(state->capability_map, resolver, *primary));

    enrage::core::Runtime runtime{
        std::make_unique<enrage::adapters::LabAdapter>(state), mods};
    assert(runtime.start());
    assert(runtime.extensions_active());
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_TICK) != 0);
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_VIRTUAL_FILES) != 0);
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_ASSETS) == 0);
    assert(runtime.plugins().plugins().size() == 1);
    assert(runtime.overlay().resolve("data/value.txt").has_value());
    assert(has_message(runtime.diagnostics(), "AssetsSymbol is not defined in the active profile"));
    runtime.tick(1.0 / 60.0);

    const auto baseline_diagnostics = runtime.diagnostics().size();
    assert(health.apply(state->capability_map, resolver, *fallback));
    runtime.tick(1.0 / 60.0);
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_TICK) != 0);
    assert(runtime.plugins().plugins().size() == 1);
    assert(has_message(runtime.diagnostics(), "fallback candidate #2"));
    assert(runtime.diagnostics().size() > baseline_diagnostics);

    const auto degraded_diagnostics = runtime.diagnostics().size();
    runtime.tick(1.0 / 60.0);
    assert(runtime.diagnostics().size() == degraded_diagnostics);

    assert(health.apply(state->capability_map, resolver, *ambiguous));
    runtime.tick(1.0 / 60.0);
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_TICK) == 0);
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_VIRTUAL_FILES) != 0);
    assert(runtime.plugins().plugins().size() == 1);
    assert(runtime.overlay().resolve("data/value.txt").has_value());
    assert(has_message(runtime.diagnostics(), "ambiguous (2 matches)"));

    assert(health.apply(state->capability_map, resolver, *missing));
    runtime.tick(1.0 / 60.0);
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_TICK) == 0);
    assert(has_message(runtime.diagnostics(), "signature not found"));

    assert(health.apply(state->capability_map, resolver, *primary));
    runtime.tick(1.0 / 60.0);
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_TICK) != 0);
    assert(runtime.plugins().plugins().size() == 1);
    assert(runtime.overlay().resolve("data/value.txt").has_value());

    runtime.stop();
    std::filesystem::remove_all(root);
}

} // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    resolver_outcomes_map_to_health();
    runtime_reacts_to_signature_health(argv[1]);
    return 0;
}
