#include "adapters/image_provider.hpp"
#include "platform/fingerprint.hpp"
#include "platform/pe_image.hpp"
#include "platform/signature/pattern.hpp"
#include "platform/signature/resolver.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

    const auto text = optional + 0xf0;
    put_name(bytes, text, ".text");
    put32(bytes, text + 8, 0x200);
    put32(bytes, text + 12, 0x1000);
    put32(bytes, text + 16, 0x200);
    put32(bytes, text + 20, 0x200);
    put32(bytes, text + 36, 0x60000020);

    bytes[0x210] = std::byte{0xDE};
    bytes[0x211] = std::byte{0xAD};
    bytes[0x212] = std::byte{0xBE};
    bytes[0x213] = std::byte{0xEF};
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
    const auto root = std::filesystem::temp_directory_path() / "enrage-image-provider-test";
    const auto path = root / "fixture.exe";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto file = file_fixture();
    write_bytes(path, file);
    const auto fingerprint = enrage::platform::fingerprint_file(path);
    const auto metadata = enrage::platform::PeImage::parse(file);
    assert(fingerprint && metadata);

    enrage::adapters::FileImageProvider file_provider{path};
    assert(file_provider.name() == "file");
    assert(file_provider.identity() == fingerprint);
    const auto file_image = file_provider.scan_image();
    assert(file_image);
    assert(file_image->layout() == enrage::platform::ImageLayout::file);

    std::vector<std::byte> loaded(0x3000);
    loaded[0x1010] = std::byte{0xDE};
    loaded[0x1011] = std::byte{0xAD};
    loaded[0x1012] = std::byte{0xBE};
    loaded[0x1013] = std::byte{0xEF};

    enrage::adapters::LoadedSnapshotImageProvider loaded_provider{
        *fingerprint,
        std::move(loaded),
        *metadata,
        0x180000000ull};
    assert(loaded_provider.name() == "loaded-snapshot");
    assert(loaded_provider.identity() == fingerprint);
    const auto loaded_image = loaded_provider.scan_image();
    assert(loaded_image);
    assert(loaded_image->layout() == enrage::platform::ImageLayout::loaded);
    assert(loaded_image->base_address() == 0x180000000ull);

    auto pattern = enrage::platform::signature::Pattern::parse("DE AD BE EF");
    assert(pattern);
    enrage::platform::signature::Resolver resolver;
    assert(resolver.add({"FixtureSymbol", {{std::move(*pattern), 0}}, ".text"}));

    const auto from_file = enrage::platform::resolve_in_image(resolver, "FixtureSymbol", *file_image);
    const auto from_loaded = enrage::platform::resolve_in_image(resolver, "FixtureSymbol", *loaded_image);
    assert(from_file.state == enrage::platform::signature::ResolutionState::unique);
    assert(from_loaded.state == enrage::platform::signature::ResolutionState::unique);
    assert(from_file.rva == 0x1010);
    assert(from_loaded.rva == 0x1010);
    assert(from_file.virtual_address == 0x140001010ull);
    assert(from_loaded.virtual_address == 0x180001010ull);

#if defined(_WIN32)
    const auto current = enrage::adapters::CurrentProcessImageProvider::create();
    assert(current);
    assert(current->name() == "current-process");
    const auto current_identity = current->identity();
    const auto disk_identity = enrage::platform::fingerprint_file(current->executable_path());
    assert(current_identity && disk_identity && *current_identity == *disk_identity);
    const auto current_image = current->scan_image();
    assert(current_image);
    assert(current_image->layout() == enrage::platform::ImageLayout::loaded);
    assert(current_image->base_address() != 0);
#else
    assert(!enrage::adapters::CurrentProcessImageProvider::create());
#endif

    std::filesystem::remove_all(root);
    return 0;
}
