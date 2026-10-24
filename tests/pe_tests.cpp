#include "platform/pe_image.hpp"

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
    put32(bytes, section + 8, 0x180);
    put32(bytes, section + 12, 0x1000);
    put32(bytes, section + 16, 0x200);
    put32(bytes, section + 20, 0x200);
    put32(bytes, section + 36, 0x60000020);

    bytes[0x200] = std::byte{0x90};
    bytes[0x201] = std::byte{0x90};
    return bytes;
}

} // namespace

int main() {
    const auto bytes = fixture();
    const auto image = enrage::platform::PeImage::parse(bytes);
    assert(image.has_value());
    assert(image->machine() == 0x8664);
    assert(image->x64());
    assert(image->timestamp() == 0x12345678);
    assert(image->image_base() == 0x140000000ull);
    assert(image->entry_rva() == 0x1000);
    assert(image->image_size() == 0x3000);
    assert(image->sections().size() == 1);

    const auto* text = image->find_section(".text");
    assert(text != nullptr);
    assert(text->virtual_address == 0x1000);
    assert(text->raw_offset == 0x200);

    const auto text_bytes = image->section_bytes(bytes, ".text");
    assert(text_bytes.has_value());
    assert(text_bytes->size() == 0x200);
    assert((*text_bytes)[0] == std::byte{0x90});
    assert(!image->section_bytes(bytes, ".missing").has_value());

    auto invalid = bytes;
    put32(invalid, 0x188 + 20, 0x380);
    put32(invalid, 0x188 + 16, 0x200);
    assert(!enrage::platform::PeImage::parse(invalid).has_value());
    return 0;
}
