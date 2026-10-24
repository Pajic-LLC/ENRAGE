#include "core/inspection.hpp"
#include "core/profile_authoring.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<std::byte> noise(std::size_t size, std::uint32_t seed) {
    std::vector<std::byte> bytes(size);
    std::uint32_t state = seed;
    for (auto& value : bytes) {
        state = state * 1664525u + 1013904223u;
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

void drafts_unique_signature_and_validates_profile_set() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-profile-authoring-test";
    const auto profiles = root / "profiles";
    const auto images = root / "images";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(profiles);
    std::filesystem::create_directories(images);

    auto reference = noise(512, 0x12345678u);
    auto target = noise(640, 0x87654321u);

    constexpr std::size_t reference_region = 80;
    constexpr std::size_t target_region = 220;
    constexpr std::size_t region_size = 140;
    constexpr std::size_t reference_symbol = 140;
    constexpr std::size_t expected_target_symbol = target_region + (reference_symbol - reference_region);

    for (std::size_t i = 0; i < region_size; ++i) {
        target[target_region + i] = reference[reference_region + i];
    }
    for (std::size_t i = 9; i < region_size; i += 17) {
        target[target_region + i] ^= std::byte{0x5a};
    }

    const auto reference_path = root / "reference.exe";
    const auto target_path = images / "target.exe";
    write_bytes(reference_path, reference);
    write_bytes(target_path, target);

    const auto drafted = enrage::core::draft_profile(
        reference_path,
        target_path,
        "fixture.target",
        {{"FixtureSymbol", reference_symbol}},
        5);
    assert(drafted.ok());
    assert(drafted.profile->symbols.size() == 1);
    const auto& symbol = drafted.profile->symbols.front();
    assert(symbol.target_offset == expected_target_symbol);
    assert(symbol.reference_match_count == 1);
    assert(symbol.target_match_count == 1);
    assert(symbol.exact_bytes >= 12);
    assert(symbol.similarity >= 0.55);
    assert(!symbol.pattern.empty());

    const auto profile_path = profiles / "fixture.enp";
    std::ofstream{profile_path, std::ios::binary | std::ios::trunc}
        << enrage::core::profile_draft_json(*drafted.profile);

    const auto validation = enrage::core::validate_profile_set(profiles, images);
    assert(validation.ok());
    assert(validation.profiles_scanned == 1);
    assert(validation.images_scanned == 1);
    assert(validation.builds.size() == 1);
    assert(validation.builds.front().valid);
    assert(validation.builds.front().symbols.size() == 1);
    assert(validation.builds.front().symbols.front().offset == expected_target_symbol);

    const auto inspection = enrage::core::inspect_executable(target_path, profiles);
    assert(inspection.has_value());
    assert(inspection->matched_profile.has_value());
    assert(*inspection->matched_profile == "fixture.target");
    assert(inspection->symbols.size() == 1);
    assert(inspection->symbols.front().state == enrage::platform::signature::ResolutionState::unique);
    assert(inspection->symbols.front().file_offset == expected_target_symbol);

    std::filesystem::remove_all(root);
}

void refuses_unrelated_target() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-profile-authoring-negative-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto reference = noise(512, 0x11111111u);
    const auto target = noise(512, 0x99999999u);
    const auto reference_path = root / "reference.exe";
    const auto target_path = root / "target.exe";
    write_bytes(reference_path, reference);
    write_bytes(target_path, target);

    const auto drafted = enrage::core::draft_profile(
        reference_path,
        target_path,
        "fixture.unrelated",
        {{"NoMatch", 200}});
    assert(!drafted.ok());
    assert(!drafted.profile.has_value());
    assert(!drafted.errors.empty());

    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    drafts_unique_signature_and_validates_profile_set();
    refuses_unrelated_target();
    return 0;
}
