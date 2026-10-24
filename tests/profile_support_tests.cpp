#include "core/build_profile_io.hpp"
#include "core/inspection.hpp"
#include "core/profile_authoring.hpp"
#include "core/profile_matrix.hpp"
#include "platform/fingerprint.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void put16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffu);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xffu);
}

void put32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::byte>((value >> (i * 8)) & 0xffu);
}

void put64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) bytes[offset + i] = static_cast<std::byte>((value >> (i * 8)) & 0xffu);
}

void put_name(std::vector<std::byte>& bytes, std::size_t offset, const char* name) {
    for (std::size_t i = 0; name[i] != '\0' && i < 8; ++i) {
        bytes[offset + i] = static_cast<std::byte>(name[i]);
    }
}

std::vector<std::byte> fixture() {
    std::vector<std::byte> bytes(0x700);
    bytes[0] = std::byte{'M'};
    bytes[1] = std::byte{'Z'};
    put32(bytes, 0x3c, 0x80);

    const std::size_t pe = 0x80;
    bytes[pe] = std::byte{'P'};
    bytes[pe + 1] = std::byte{'E'};
    const auto coff = pe + 4;
    put16(bytes, coff, 0x8664);
    put16(bytes, coff + 2, 2);
    put32(bytes, coff + 4, 0x12345678);
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

    for (const auto offset : {std::size_t{0x210}, std::size_t{0x410}}) {
        bytes[offset] = std::byte{0xDE};
        bytes[offset + 1] = std::byte{0xAD};
        bytes[offset + 2] = std::byte{0xBE};
        bytes[offset + 3] = std::byte{0xEF};
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
    const auto root = std::filesystem::temp_directory_path() / "enrage-profile-support-test";
    const auto profiles = root / "profiles";
    const auto images = root / "images";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(profiles);
    std::filesystem::create_directories(images);

    const auto image_path = images / "fixture.exe";
    write_bytes(image_path, fixture());
    const auto fingerprint = enrage::platform::fingerprint_file(image_path);
    assert(fingerprint.has_value());

    enrage::core::ProfileDraft draft{
        .id = "fixture.scoped",
        .fingerprint = *fingerprint,
        .capabilities = 3,
        .symbols = {{
            .name = "FixtureSymbol",
            .reference_offset = 0x210,
            .target_offset = 0x210,
            .pattern = "DE AD BE EF",
            .adjustment = 0,
            .similarity = 1.0,
            .exact_bytes = 4,
            .wildcard_bytes = 0,
            .reference_match_count = 2,
            .target_match_count = 2,
            .section = std::string{".text"},
        }},
    };

    const auto serialized = enrage::core::serialize_profile_draft(draft);
    assert(serialized.find("\"version\": 2") != std::string::npos);
    assert(serialized.find("\"region\": \".text\"") != std::string::npos);
    const auto parsed = enrage::core::parse_build_profile(serialized);
    assert(parsed.ok());
    assert(parsed.profile->symbols[0].region == ".text");

    const auto profile_path = profiles / "fixture.enp";
    std::ofstream{profile_path} << serialized;

    const auto validation = enrage::core::validate_profile_set_strict(profiles, images);
    assert(validation.ok());
    assert(validation.builds.size() == 1);
    assert(validation.builds[0].symbols.size() == 1);
    assert(validation.builds[0].symbols[0].state == enrage::platform::signature::ResolutionState::unique);
    assert(validation.builds[0].symbols[0].offset == 0x210);

    const auto matrix = enrage::core::build_profile_matrix(profiles, images);
    assert(matrix.ok());
    assert(matrix.rows.size() == 1);
    assert(matrix.rows[0].cells.size() == 1);
    assert(matrix.rows[0].cells[0].state == enrage::platform::signature::ResolutionState::unique);
    assert(matrix.rows[0].cells[0].offset == 0x210);

    const auto inspection = enrage::core::inspect_executable(image_path, profiles);
    assert(inspection.has_value());
    assert(inspection->matched_profile == "fixture.scoped");
    assert(inspection->symbols.size() == 1);
    assert(inspection->symbols[0].state == enrage::platform::signature::ResolutionState::unique);
    assert(inspection->symbols[0].file_offset == 0x210);
    assert(inspection->symbols[0].rva == 0x1010);
    assert(inspection->symbols[0].virtual_address == 0x140001010ull);
    assert(inspection->symbols[0].section == ".text");

    std::filesystem::remove_all(root);
    return 0;
}
