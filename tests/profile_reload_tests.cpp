#include "adapters/profile_adapter.hpp"
#include "core/runtime.hpp"
#include "enrage/sdk.h"
#include "platform/fingerprint.hpp"

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

void put16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffu);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xffu);
}

void put32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (i * 8u)) & 0xffu);
    }
}

void put64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (i * 8u)) & 0xffu);
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

std::vector<std::byte> image_fixture() {
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

    put_pattern(bytes, 0x210, {0xAA, 0xBB, 0xCC, 0xDD});
    put_pattern(bytes, 0x260, {0x55, 0x66, 0x77, 0x88});
    return bytes;
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(output);
}

void write_profile(
    const std::filesystem::path& path,
    std::string_view id,
    const enrage::platform::FileFingerprint& fingerprint,
    std::string_view scheduler_pattern) {
    std::ofstream profile{path, std::ios::binary | std::ios::trunc};
    assert(profile);
    profile
        << "{\"version\":3,\"id\":\"" << id << "\","
        << "\"fingerprint\":{\"sha256\":\"" << fingerprint.hex()
        << "\",\"size\":" << fingerprint.size << "},"
        << "\"capabilities\":3,"
        << "\"symbols\":["
        << "{\"name\":\"SchedulerSymbol\",\"region\":\".text\","
        << "\"candidates\":[{\"pattern\":\"" << scheduler_pattern << "\",\"adjustment\":0}]},"
        << "{\"name\":\"VirtualFilesSymbol\",\"region\":\".text\","
        << "\"candidates\":[{\"pattern\":\"55 66 77 88\",\"adjustment\":0}]}],"
        << "\"anchors\":["
        << "{\"name\":\"scheduler\",\"symbol\":\"SchedulerSymbol\"},"
        << "{\"name\":\"virtual-files\",\"symbol\":\"VirtualFilesSymbol\"}],"
        << "\"requirements\":["
        << "{\"capability\":1,\"anchors\":[\"scheduler\"]},"
        << "{\"capability\":2,\"anchors\":[\"virtual-files\"]}]}";
    assert(profile);
}

const enrage::adapters::AnchorHealth* find_anchor(
    const enrage::adapters::AdapterHealthSnapshot& snapshot,
    std::string_view name) {
    for (const auto& anchor : snapshot.anchors) {
        if (anchor.name == name) return &anchor;
    }
    return nullptr;
}

} // namespace

int main() {
    using enrage::adapters::AnchorHealthState;
    using enrage::adapters::EnvironmentState;

    const auto root = std::filesystem::temp_directory_path() / "enrage-profile-reload-test";
    const auto profiles = root / "profiles";
    const auto mods = root / "mods";
    const auto mod = mods / "example";
    const auto image_path = root / "host.exe";
    const auto profile_path = profiles / "fixture.enp";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(profiles);
    std::filesystem::create_directories(mod / "files" / "data");
    std::ofstream{mod / "mod.json"} << R"({"id":"example.reload"})";
    std::ofstream{mod / "files" / "data" / "value.txt"} << "active";

    auto image = image_fixture();
    write_bytes(image_path, image);
    const auto original_fingerprint = enrage::platform::fingerprint_file(image_path);
    assert(original_fingerprint);
    write_profile(profile_path, "fixture.original", *original_fingerprint, "AA BB CC DD");

    enrage::core::Runtime runtime{
        std::make_unique<enrage::adapters::ProfileAdapter>(
            image_path,
            profiles,
            EnvironmentState::local),
        mods};
    assert(runtime.start());
    assert(runtime.extensions_active());
    assert(runtime.capabilities() == (ENRAGE_CAPABILITY_TICK | ENRAGE_CAPABILITY_VIRTUAL_FILES));
    assert(runtime.overlay().resolve("data/value.txt").has_value());
    assert(runtime.adapter().build().id == "fixture.original");
    assert(runtime.adapter().build().fingerprint == original_fingerprint);

    // A broken replacement for the same exact image must roll back to the
    // already-verified in-memory support state.
    std::ofstream{profile_path, std::ios::binary | std::ios::trunc} << "{not-json";
    assert(!runtime.reload_support());
    assert(runtime.extensions_active());
    assert(runtime.capabilities() == (ENRAGE_CAPABILITY_TICK | ENRAGE_CAPABILITY_VIRTUAL_FILES));
    assert(runtime.overlay().resolve("data/value.txt").has_value());
    assert(runtime.adapter().build().id == "fixture.original");

    // A valid same-build profile may intentionally degrade one subsystem.
    write_profile(profile_path, "fixture.degraded", *original_fingerprint, "10 20 30 40");
    assert(runtime.reload_support());
    assert(runtime.adapter().build().id == "fixture.degraded");
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_TICK) == 0);
    assert((runtime.capabilities() & ENRAGE_CAPABILITY_VIRTUAL_FILES) != 0);
    assert(runtime.extensions_active());
    assert(runtime.overlay().resolve("data/value.txt").has_value());
    const auto* scheduler = find_anchor(runtime.health(), "scheduler");
    assert(scheduler && scheduler->state == AnchorHealthState::missing);

    write_profile(profile_path, "fixture.recovered", *original_fingerprint, "AA BB CC DD");
    assert(runtime.reload_support());
    assert(runtime.capabilities() == (ENRAGE_CAPABILITY_TICK | ENRAGE_CAPABILITY_VIRTUAL_FILES));
    assert(runtime.extensions_active());

    // If the host image itself changes, old support must never survive under
    // the new identity. The runtime drops to unknown and disables extensions.
    image[0x2f0] ^= std::byte{0x5a};
    write_bytes(image_path, image);
    const auto updated_fingerprint = enrage::platform::fingerprint_file(image_path);
    assert(updated_fingerprint && *updated_fingerprint != *original_fingerprint);
    assert(runtime.reload_support());
    assert(runtime.adapter().build().id == "unknown");
    assert(runtime.adapter().build().fingerprint == updated_fingerprint);
    assert(runtime.adapter().environment_state() == EnvironmentState::unknown);
    assert(runtime.capabilities() == 0);
    assert(!runtime.extensions_active());
    assert(!runtime.overlay().resolve("data/value.txt").has_value());

    // Dropping support data for that exact new build unlocks the same running
    // runtime without replacing the loader binary.
    write_profile(profile_path, "fixture.updated", *updated_fingerprint, "AA BB CC DD");
    assert(runtime.reload_support());
    assert(runtime.adapter().build().id == "fixture.updated");
    assert(runtime.adapter().environment_state() == EnvironmentState::local);
    assert(runtime.capabilities() == (ENRAGE_CAPABILITY_TICK | ENRAGE_CAPABILITY_VIRTUAL_FILES));
    assert(runtime.extensions_active());
    assert(runtime.overlay().resolve("data/value.txt").has_value());

    runtime.stop();
    std::filesystem::remove_all(root);
    return 0;
}
