#include "core/inspection.hpp"
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

std::vector<std::byte> fixture() {
    std::vector<std::byte> bytes(0x400);
    bytes[0] = std::byte{'M'};
    bytes[1] = std::byte{'Z'};
    put32(bytes, 0x3c, 0x80);

    const std::size_t pe = 0x80;
    bytes[pe] = std::byte{'P'};
    bytes[pe + 1] = std::byte{'E'};

    const auto coff = pe + 4;
    put16(bytes, coff, 0x8664);
    put16(bytes, coff + 2, 1);
    put32(bytes, coff + 4, 0x11223344);
    put16(bytes, coff + 16, 0xf0);

    const auto optional = coff + 20;
    put16(bytes, optional, 0x20b);
    put32(bytes, optional + 16, 0x1000);
    put64(bytes, optional + 24, 0x140000000ull);
    put32(bytes, optional + 56, 0x3000);

    const auto section = optional + 0xf0;
    const char name[] = ".text";
    for (std::size_t i = 0; i < 5; ++i) bytes[section + i] = static_cast<std::byte>(name[i]);
    put32(bytes, section + 8, 0x180);
    put32(bytes, section + 12, 0x1000);
    put32(bytes, section + 16, 0x200);
    put32(bytes, section + 20, 0x200);
    put32(bytes, section + 36, 0x60000020);

    bytes[0x210] = std::byte{0xde};
    bytes[0x211] = std::byte{0xad};
    bytes[0x212] = std::byte{0xbe};
    bytes[0x213] = std::byte{0xef};
    return bytes;
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-inspection-test";
    const auto image = root / "fixture.exe";
    const auto profiles = root / "profiles";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(profiles);

    const auto bytes = fixture();
    write_bytes(image, bytes);
    const auto fingerprint = enrage::platform::fingerprint_file(image);
    assert(fingerprint.has_value());

    std::ofstream{profiles / "fixture.enp"}
        << "{"
           "\"version\":1,"
           "\"id\":\"fixture.inspect\","
           "\"fingerprint\":{\"sha256\":\"" << fingerprint->hex()
        << "\",\"size\":" << fingerprint->size << "},"
           "\"capabilities\":9,"
           "\"symbols\":[{"
             "\"name\":\"fixture.symbol\","
             "\"candidates\":[{\"pattern\":\"DE AD BE EF\",\"adjustment\":1}]"
           "}]"
           "}";

    const auto report = enrage::core::inspect_executable(image, profiles);
    assert(report.has_value());
    assert(report->pe_valid);
    assert(report->x64);
    assert(report->machine == 0x8664);
    assert(report->sections.size() == 1);
    assert(report->imported_profile_count == 1);
    assert(report->profile_errors.empty());
    assert(report->matched_profile == "fixture.inspect");
    assert(report->capabilities == 9);
    assert(report->symbols.size() == 1);

    const auto& symbol = report->symbols[0];
    assert(symbol.state == enrage::platform::signature::ResolutionState::unique);
    assert(symbol.match_count == 1);
    assert(symbol.file_offset == 0x211);
    assert(symbol.section == ".text");
    assert(symbol.rva == 0x1011);
    assert(symbol.virtual_address == 0x140001011ull);

    const auto json = enrage::core::inspection_report_json(*report);
    assert(json.find("\"schema\":1") != std::string::npos);
    assert(json.find("\"matched\":\"fixture.inspect\"") != std::string::npos);
    assert(json.find("\"fileOffset\":529") != std::string::npos);
    assert(json.find("\"rva\":4113") != std::string::npos);

    const auto unmatched = enrage::core::inspect_executable(image);
    assert(unmatched.has_value());
    assert(unmatched->pe_valid);
    assert(!unmatched->matched_profile.has_value());
    assert(unmatched->symbols.empty());

    std::filesystem::remove_all(root);
    return 0;
}
