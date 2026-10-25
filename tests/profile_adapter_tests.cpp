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

const enrage::adapters::AnchorHealth* find_anchor(
    const enrage::adapters::AdapterHealthSnapshot& snapshot,
    std::string_view name) {
    for (const auto& anchor : snapshot.anchors) {
        if (anchor.name == name) return &anchor;
    }
    return nullptr;
}

bool diagnostic_contains(
    const std::vector<enrage::core::Diagnostic>& diagnostics,
    std::string_view text) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.message.find(text) != std::string::npos) return true;
    }
    return false;
}

} // namespace

int main() {
    using enrage::adapters::AnchorHealthState;
    using enrage::adapters::EnvironmentState;

    const auto root = std::filesystem::temp_directory_path() / "enrage-profile-adapter-test";
    const auto profiles = root / "profiles";
    const auto mods = root / "mods";
    const auto image_path = root / "host.exe";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(profiles);
    std::filesystem::create_directories(mods);

    auto image = image_fixture();
    write_bytes(image_path, image);
    const auto fingerprint = enrage::platform::fingerprint_file(image_path);
    assert(fingerprint);

    std::ofstream profile{profiles / "fixture.enp"};
    profile
        << "{\"version\":3,\"id\":\"fixture.exact\","
        << "\"fingerprint\":{\"sha256\":\"" << fingerprint->hex()
        << "\",\"size\":" << fingerprint->size << "},"
        << "\"capabilities\":3,"
        << "\"symbols\":["
        << "{\"name\":\"SchedulerSymbol\",\"region\":\".text\","
        << "\"candidates\":[{\"pattern\":\"AA BB CC DD\",\"adjustment\":0}]},"
        << "{\"name\":\"VirtualFilesSymbol\",\"region\":\".text\","
        << "\"candidates\":[{\"pattern\":\"55 66 77 88\",\"adjustment\":0}]}],"
        << "\"anchors\":["
        << "{\"name\":\"scheduler\",\"symbol\":\"SchedulerSymbol\"},"
        << "{\"name\":\"virtual-files\",\"symbol\":\"VirtualFilesSymbol\"}],"
        << "\"requirements\":["
        << "{\"capability\":1,\"anchors\":[\"scheduler\"]},"
        << "{\"capability\":2,\"anchors\":[\"virtual-files\"]}]}";
    profile.close();

    {
        enrage::adapters::ProfileAdapter adapter{image_path, profiles, EnvironmentState::local};
        assert(adapter.probe());
        assert(adapter.profile_matched());
        assert(adapter.build().id == "fixture.exact");
        assert(adapter.environment_state() == EnvironmentState::local);
        assert(adapter.capabilities() == (ENRAGE_CAPABILITY_TICK | ENRAGE_CAPABILITY_VIRTUAL_FILES));
        assert(adapter.profile_errors().empty());
        const auto health = adapter.health_snapshot();
        assert(find_anchor(health, "scheduler"));
        assert(find_anchor(health, "scheduler")->state == AnchorHealthState::resolved);
        assert(find_anchor(health, "virtual-files"));
        assert(find_anchor(health, "virtual-files")->state == AnchorHealthState::resolved);
        assert(find_anchor(health, "profile"));
        assert(find_anchor(health, "profile")->state == AnchorHealthState::resolved);
    }

    {
        enrage::core::Runtime runtime{
            std::make_unique<enrage::adapters::ProfileAdapter>(
                image_path,
                profiles,
                EnvironmentState::local),
            mods};
        assert(runtime.start());
        assert(runtime.running());
        assert(runtime.extensions_active());
        assert(runtime.capabilities() == (ENRAGE_CAPABILITY_TICK | ENRAGE_CAPABILITY_VIRTUAL_FILES));
        assert(runtime.adapter().build().id == "fixture.exact");
        runtime.stop();
    }

    image[0x2f0] ^= std::byte{0x5a};
    const auto unknown_path = root / "unknown.exe";
    write_bytes(unknown_path, image);

    {
        auto adapter = std::make_unique<enrage::adapters::ProfileAdapter>(
            unknown_path,
            profiles,
            EnvironmentState::local);
        auto* raw = adapter.get();
        enrage::core::Runtime runtime{std::move(adapter), mods};
        assert(runtime.start());
        assert(!raw->profile_matched());
        assert(runtime.adapter().environment_state() == EnvironmentState::unknown);
        assert(runtime.capabilities() == 0);
        assert(!runtime.extensions_active());
        const auto* profile_health = find_anchor(runtime.health(), "profile");
        assert(profile_health && profile_health->state == AnchorHealthState::missing);
        assert(diagnostic_contains(runtime.diagnostics(), "no exact build profile matches host image fingerprint"));
        runtime.stop();
    }

    std::filesystem::remove_all(root);
    return 0;
}
