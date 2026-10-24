#include "core/profile_propagation.hpp"
#include "platform/fingerprint.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<std::byte> noise(std::size_t size, std::uint32_t seed) {
    std::vector<std::byte> bytes(size);
    std::uint32_t state = seed;
    for (auto& value : bytes) {
        state = state * 1103515245u + 12345u;
        value = static_cast<std::byte>((state >> 16) & 0xffu);
    }
    return bytes;
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(output);
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-profile-propagation-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    auto reference = noise(640, 0x13572468u);
    auto target = noise(768, 0x24681357u);

    constexpr std::size_t reference_region = 100;
    constexpr std::size_t target_region = 260;
    constexpr std::size_t region_size = 180;
    constexpr std::size_t reference_symbol = 150;
    constexpr std::size_t expected_target_symbol = target_region + (reference_symbol - reference_region);

    const std::byte anchor[]{
        std::byte{0x10}, std::byte{0x21}, std::byte{0x32}, std::byte{0x43},
        std::byte{0x54}, std::byte{0x65}, std::byte{0x76}, std::byte{0x87},
        std::byte{0x98}, std::byte{0xA9}, std::byte{0xBA}, std::byte{0xCB},
    };
    for (std::size_t i = 0; i < std::size(anchor); ++i) reference[140 + i] = anchor[i];

    for (std::size_t i = 0; i < region_size; ++i) {
        target[target_region + i] = reference[reference_region + i];
    }
    for (std::size_t i = 7; i < region_size; i += 19) {
        target[target_region + i] ^= std::byte{0x3c};
    }

    const auto reference_path = root / "reference.exe";
    const auto target_path = root / "target.exe";
    const auto profile_path = root / "reference.enp";
    write_bytes(reference_path, reference);
    write_bytes(target_path, target);

    const auto fingerprint = enrage::platform::fingerprint_file(reference_path);
    assert(fingerprint.has_value());
    std::ofstream{profile_path}
        << "{\"version\":1,\"id\":\"fixture.reference\","
        << "\"fingerprint\":{\"sha256\":\"" << fingerprint->hex()
        << "\",\"size\":" << fingerprint->size << "},"
        << "\"capabilities\":9,\"symbols\":[{\"name\":\"PropagatedSymbol\","
        << "\"candidates\":[{\"pattern\":\"10 21 32 43 54 65 76 87 98 A9 BA CB\","
        << "\"adjustment\":10}]}]}";

    const auto propagated = enrage::core::propagate_profile(
        reference_path,
        target_path,
        profile_path,
        "fixture.target");
    assert(propagated.ok());
    assert(propagated.profile->capabilities == 9);
    assert(propagated.profile->symbols.size() == 1);
    assert(propagated.profile->symbols.front().name == "PropagatedSymbol");
    assert(propagated.profile->symbols.front().target_offset == expected_target_symbol);
    assert(propagated.profile->symbols.front().target_match_count == 1);

    auto modified_reference = reference;
    modified_reference[0] ^= std::byte{0xff};
    const auto modified_path = root / "modified.exe";
    write_bytes(modified_path, modified_reference);
    const auto rejected = enrage::core::propagate_profile(
        modified_path,
        target_path,
        profile_path,
        "fixture.rejected");
    assert(!rejected.ok());
    assert(!rejected.errors.empty());

    std::filesystem::remove_all(root);
    return 0;
}
