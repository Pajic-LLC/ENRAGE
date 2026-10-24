#include "platform/pe_image.hpp"
#include "platform/scan_image.hpp"
#include "platform/signature/pattern.hpp"
#include "platform/signature/resolver.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
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

std::vector<std::byte> file_fixture() {
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

    const auto section = optional + 0xf0;
    const char name[] = ".text";
    for (std::size_t i = 0; i < 5; ++i) bytes[section + i] = static_cast<std::byte>(name[i]);
    put32(bytes, section + 8, 0x200);
    put32(bytes, section + 12, 0x1000);
    put32(bytes, section + 16, 0x200);
    put32(bytes, section + 20, 0x200);
    put32(bytes, section + 36, 0x60000020);

    bytes[0x210] = std::byte{0xDE};
    bytes[0x211] = std::byte{0xAD};
    bytes[0x212] = std::byte{0xBE};
    bytes[0x213] = std::byte{0xEF};
    return bytes;
}

} // namespace

int main() {
    const auto file = file_fixture();
    const auto metadata = enrage::platform::PeImage::parse(file);
    assert(metadata.has_value());

    const auto file_image = enrage::platform::ScanImage::from_file(file);
    assert(file_image.has_value());
    assert(file_image->layout() == enrage::platform::ImageLayout::file);
    assert(file_image->base_address() == 0x140000000ull);

    std::vector<std::byte> loaded(0x3000);
    loaded[0x1010] = std::byte{0xDE};
    loaded[0x1011] = std::byte{0xAD};
    loaded[0x1012] = std::byte{0xBE};
    loaded[0x1013] = std::byte{0xEF};
    const auto loaded_image = enrage::platform::ScanImage::from_loaded_snapshot(
        loaded,
        *metadata,
        0x180000000ull);
    assert(loaded_image.has_value());
    assert(loaded_image->layout() == enrage::platform::ImageLayout::loaded);

    auto pattern = enrage::platform::signature::Pattern::parse("DE AD BE EF");
    assert(pattern.has_value());
    enrage::platform::signature::Resolver resolver;
    assert(resolver.add({"FixtureSymbol", {{std::move(*pattern), 0}}}));

    const auto from_file = enrage::platform::resolve_in_image(resolver, "FixtureSymbol", *file_image, ".text");
    assert(from_file.state == enrage::platform::signature::ResolutionState::unique);
    assert(from_file.storage_offset == 0x210);
    assert(from_file.rva == 0x1010);
    assert(from_file.virtual_address == 0x140001010ull);
    assert(from_file.region == ".text");

    const auto from_loaded = enrage::platform::resolve_in_image(resolver, "FixtureSymbol", *loaded_image, ".text");
    assert(from_loaded.state == enrage::platform::signature::ResolutionState::unique);
    assert(from_loaded.storage_offset == 0x1010);
    assert(from_loaded.rva == 0x1010);
    assert(from_loaded.virtual_address == 0x180001010ull);
    assert(from_loaded.region == ".text");

    return 0;
}
