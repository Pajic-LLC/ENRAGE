#include "adapters/profile_runtime.hpp"
#include "core/build_catalog.hpp"
#include "core/build_profile_v3.hpp"
#include "enrage/sdk.h"
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

} // namespace

int main() {
    parses_and_round_trips();
    rejects_invalid_topology();
    profile_drives_runtime_topology();
    return 0;
}
