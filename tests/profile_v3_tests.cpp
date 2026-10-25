#include "adapters/profile_runtime.hpp"
#include "core/build_catalog.hpp"
#include "core/build_profile_v3.hpp"
#include "enrage/sdk.h"
#include "platform/pe_image.hpp"
#include "platform/scan_image.hpp"
#include "platform/signature/resolver.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

void put16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffu);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xffu);
}

void put32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xffu);
    }
}

void put64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xffu);
    }
}

void put_name(std::vector<std::byte>& bytes, std::size_t offset, const char* name) {
    for (std::size_t index = 0; name[index] != '\0' && index < 8; ++index) {
        bytes[offset + index] = static_cast<std::byte>(name[index]);
    }
}

void put_pattern(std::vector<std::byte>& bytes, std::size_t offset, std::initializer_list<unsigned> values) {
    for (const auto value : values) bytes[offset++] = static_cast<std::byte>(value);
}

std::vector<std::byte> image(bool primary, bool fallback, bool duplicate_primary = false) {
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

    if (primary) put_pattern(bytes, 0x210, {0xAA, 0xBB, 0xCC, 0xDD});
    if (duplicate_primary) put_pattern(bytes, 0x240, {0xAA, 0xBB, 0xCC, 0xDD});
    if (fallback) put_pattern(bytes, 0x260, {0x11, 0x22, 0x33, 0x44});
    put_pattern(bytes, 0x2a0, {0x55, 0x66, 0x77, 0x88});
    return bytes;
}

std::vector<std::byte> relative_image() {
    std::vector<std::byte> bytes(0x600);
    bytes[0] = std::byte{'M'};
    bytes[1] = std::byte{'Z'};
    put32(bytes, 0x3c, 0x80);
    const std::size_t pe = 0x80;
    bytes[pe] = std::byte{'P'};
    bytes[pe + 1] = std::byte{'E'};
    const auto coff = pe + 4;
    put16(bytes, coff, 0x8664);
    put16(bytes, coff + 2, 2);
    put16(bytes, coff + 16, 0xf0);
    const auto optional = coff + 20;
    put16(bytes, optional, 0x20b);
    put32(bytes, optional + 16, 0x1000);
    put64(bytes, optional + 24, 0x140000000ull);
    put32(bytes, optional + 56, 0x4000);

    const auto text = optional + 0xf0;
    put_name(bytes, text, ".text");
    put32(bytes, text + 8, 0x200);
    put32(bytes, text + 12, 0x1000);
    put32(bytes, text + 16, 0x200);
    put32(bytes, text + 20, 0x200);
    put32(bytes, text + 36, 0x60000020);

    const auto rdata = text + 40;
    put_name(bytes, rdata, ".rdata");
    put32(bytes, rdata + 8, 0x200);
    put32(bytes, rdata + 12, 0x2000);
    put32(bytes, rdata + 16, 0x200);
    put32(bytes, rdata + 20, 0x400);
    put32(bytes, rdata + 36, 0x40000040);

    // 0x1010: 48 8D 05 <rel32>, instruction end 0x1017, target 0x2010.
    put_pattern(bytes, 0x210, {0x48, 0x8D, 0x05});
    put32(bytes, 0x213, 0x00000ff9u);
    put_pattern(bytes, 0x410, {0xCA, 0xFE, 0xBA, 0xBE});
    return bytes;
}

const enrage::adapters::AnchorHealth* anchor(
    const enrage::adapters::AdapterHealthSnapshot& snapshot,
    std::string_view name) {
    for (const auto& candidate : snapshot.anchors) {
        if (candidate.name == name) return &candidate;
    }
    return nullptr;
}

std::string profile_json() {
    return R"({
  "version": 3,
  "id": "fixture.v3",
  "fingerprint": {"sha256": "0000000000000000000000000000000000000000000000000000000000000000", "size": 1280},
  "capabilities": 3,
  "symbols": [
    {"name": "SchedulerSymbol", "region": ".text", "candidates": [
      {"pattern": "AA BB CC DD", "adjustment": 0},
      {"pattern": "11 22 33 44", "adjustment": 0}
    ]},
    {"name": "VirtualFilesSymbol", "region": ".text", "candidates": [
      {"pattern": "55 66 77 88", "adjustment": 0}
    ]}
  ],
  "anchors": [
    {"name": "scheduler", "symbol": "SchedulerSymbol"},
    {"name": "virtual-files", "symbol": "VirtualFilesSymbol"}
  ],
  "requirements": [
    {"capability": 1, "anchors": ["scheduler"]},
    {"capability": 2, "anchors": ["virtual-files"]}
  ]
})";
}

std::string relative_profile_json(int version = 4) {
    return std::string{"{\n"}
        + "  \"version\": " + std::to_string(version) + ",\n"
        + "  \"id\": \"fixture.relative\",\n"
        + "  \"fingerprint\": {\"sha256\": \"0000000000000000000000000000000000000000000000000000000000000000\", \"size\": 1536},\n"
        + "  \"capabilities\": 0,\n"
        + "  \"symbols\": [{\"name\": \"ReferencedData\", \"region\": \".text\", \"candidates\": ["
        + "{\"pattern\": \"48 8D 05 ?? ?? ?? ??\", \"adjustment\": 0, \"mode\": \"relative32\", "
        + "\"displacementOffset\": 3, \"instructionSize\": 7}]}]\n"
        + "}";
}

void parses_and_round_trips() {
    auto parsed = enrage::core::parse_build_profile_v3(profile_json(), "fixture.enp");
    assert(parsed.ok());
    assert(parsed.profile->anchors.size() == 2);
    assert(parsed.profile->requirements.size() == 2);
    assert(parsed.profile->symbols.size() == 2);

    const auto serialized = enrage::core::serialize_build_profile_v3(*parsed.profile);
    assert(serialized.find("\"version\": 3") != std::string::npos);
    assert(serialized.find("\"scheduler\"") != std::string::npos);
    assert(serialized.find("AA BB CC DD") != std::string::npos);

    auto reparsed = enrage::core::parse_build_profile_v3(serialized, "roundtrip.enp");
    assert(reparsed.ok());
    assert(reparsed.profile->anchors.size() == 2);
    assert(reparsed.profile->requirements.size() == 2);
}

void rejects_invalid_topology() {
    auto unknown_symbol = profile_json();
    const auto symbol_pos = unknown_symbol.find("\"SchedulerSymbol\"}", unknown_symbol.find("\"anchors\""));
    assert(symbol_pos != std::string::npos);
    unknown_symbol.replace(symbol_pos, std::string{"\"SchedulerSymbol\"}"}.size(), "\"MissingSymbol\"}");
    auto parsed = enrage::core::parse_build_profile_v3(unknown_symbol);
    assert(!parsed.ok());
    assert(parsed.error && parsed.error->message.find("unknown symbol") != std::string::npos);

    auto unknown_anchor = profile_json();
    const auto requirement_pos = unknown_anchor.find("\"scheduler\"]", unknown_anchor.find("\"requirements\""));
    assert(requirement_pos != std::string::npos);
    unknown_anchor.replace(requirement_pos, std::string{"\"scheduler\"]"}.size(), "\"missing-anchor\"]");
    parsed = enrage::core::parse_build_profile_v3(unknown_anchor);
    assert(!parsed.ok());
    assert(parsed.error && parsed.error->message.find("unknown anchor") != std::string::npos);

    auto old_version = profile_json();
    old_version.replace(old_version.find("\"version\": 3"), std::string{"\"version\": 3"}.size(), "\"version\": 2");
    parsed = enrage::core::parse_build_profile_v3(old_version);
    assert(!parsed.ok());
    assert(parsed.error && parsed.error->message.find("version 3") != std::string::npos);
}

void profile_drives_runtime_topology() {
    auto parsed = enrage::core::parse_build_profile_v3(profile_json());
    assert(parsed.ok());

    enrage::platform::signature::Resolver resolver;
    for (const auto& symbol : parsed.profile->symbols) assert(resolver.add(symbol));

    auto topology = enrage::adapters::ProfileRuntimeTopology::from_profile(*parsed.profile);
    assert(topology);
    assert(topology->advertised_capabilities() ==
           (ENRAGE_CAPABILITY_TICK | ENRAGE_CAPABILITY_VIRTUAL_FILES));

    const auto primary = enrage::platform::ScanImage::from_file(image(true, false));
    assert(primary && topology->evaluate(resolver, *primary));
    assert(topology->capabilities() ==
           (ENRAGE_CAPABILITY_TICK | ENRAGE_CAPABILITY_VIRTUAL_FILES));
    auto snapshot = topology->health_snapshot();
    assert(anchor(snapshot, "scheduler"));
    assert(anchor(snapshot, "scheduler")->state == enrage::adapters::AnchorHealthState::resolved);

    const auto fallback = enrage::platform::ScanImage::from_file(image(false, true));
    assert(fallback && topology->evaluate(resolver, *fallback));
    assert((topology->capabilities() & ENRAGE_CAPABILITY_TICK) != 0);
    snapshot = topology->health_snapshot();
    assert(anchor(snapshot, "scheduler")->state == enrage::adapters::AnchorHealthState::degraded);
    assert(anchor(snapshot, "scheduler")->reason.find("fallback candidate #2") != std::string::npos);

    const auto ambiguous = enrage::platform::ScanImage::from_file(image(true, false, true));
    assert(ambiguous && topology->evaluate(resolver, *ambiguous));
    assert((topology->capabilities() & ENRAGE_CAPABILITY_TICK) == 0);
    assert((topology->capabilities() & ENRAGE_CAPABILITY_VIRTUAL_FILES) != 0);
    snapshot = topology->health_snapshot();
    assert(anchor(snapshot, "scheduler")->state == enrage::adapters::AnchorHealthState::missing);
}

void relative_targets_round_trip_and_cross_regions() {
    auto parsed = enrage::core::parse_build_profile_v3(relative_profile_json(), "relative.enp");
    assert(parsed.ok());
    assert(parsed.profile->symbols.size() == 1);
    const auto& candidate = parsed.profile->symbols[0].candidates[0];
    assert(candidate.address_mode == enrage::platform::signature::AddressMode::relative32);
    assert(candidate.displacement_offset == 3);
    assert(candidate.instruction_size == 7);

    const auto serialized = enrage::core::serialize_build_profile_v3(*parsed.profile);
    assert(serialized.find("\"version\": 4") != std::string::npos);
    assert(serialized.find("\"mode\": \"relative32\"") != std::string::npos);
    auto reparsed = enrage::core::parse_build_profile_v3(serialized, "relative-roundtrip.enp");
    assert(reparsed.ok());

    enrage::platform::signature::Resolver resolver;
    assert(resolver.add(reparsed.profile->symbols[0]));

    const auto file = relative_image();
    const auto file_image = enrage::platform::ScanImage::from_file(file);
    assert(file_image);
    const auto file_result = enrage::platform::resolve_in_image(resolver, "ReferencedData", *file_image);
    assert(file_result.state == enrage::platform::signature::ResolutionState::unique);
    assert(file_result.storage_offset == 0x410);
    assert(file_result.rva == 0x2010);
    assert(file_result.virtual_address == 0x140002010ull);
    assert(file_result.region == ".rdata");

    const auto metadata = enrage::platform::PeImage::parse(file);
    assert(metadata);
    std::vector<std::byte> loaded(0x4000);
    put_pattern(loaded, 0x1010, {0x48, 0x8D, 0x05});
    put32(loaded, 0x1013, 0x00000ff9u);
    put_pattern(loaded, 0x2010, {0xCA, 0xFE, 0xBA, 0xBE});
    const auto loaded_image = enrage::platform::ScanImage::from_loaded_snapshot(
        loaded,
        *metadata,
        0x180000000ull);
    assert(loaded_image);
    const auto loaded_result = enrage::platform::resolve_in_image(resolver, "ReferencedData", *loaded_image);
    assert(loaded_result.state == enrage::platform::signature::ResolutionState::unique);
    assert(loaded_result.storage_offset == 0x2010);
    assert(loaded_result.rva == 0x2010);
    assert(loaded_result.virtual_address == 0x180002010ull);
    assert(loaded_result.region == ".rdata");

    const auto old_version = enrage::core::parse_build_profile_v3(relative_profile_json(3), "relative-v3.enp");
    assert(!old_version.ok());
    assert(old_version.error && old_version.error->message.find("version 4") != std::string::npos);

    auto invalid = relative_profile_json();
    const auto offset = invalid.find("\"instructionSize\": 7");
    assert(offset != std::string::npos);
    invalid.replace(offset, std::string{"\"instructionSize\": 7"}.size(), "\"instructionSize\": 4");
    const auto invalid_result = enrage::core::parse_build_profile_v3(invalid, "relative-invalid.enp");
    assert(!invalid_result.ok());
}

} // namespace

int main() {
    parses_and_round_trips();
    rejects_invalid_topology();
    profile_drives_runtime_topology();
    relative_targets_round_trip_and_cross_regions();
    return 0;
}
