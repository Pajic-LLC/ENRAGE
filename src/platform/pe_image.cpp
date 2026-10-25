#include "platform/pe_image.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace enrage::platform {
namespace {

constexpr std::uint32_t section_mem_execute = 0x20000000u;
constexpr std::uint32_t section_mem_read = 0x40000000u;
constexpr std::uint32_t section_mem_write = 0x80000000u;

std::optional<std::uint16_t> u16(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2) return std::nullopt;
    const auto low = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset]));
    const auto high = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1]));
    return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8));
}

std::optional<std::uint32_t> u32(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) return std::nullopt;
    std::uint32_t value{};
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + i])) << (i * 8);
    }
    return value;
}

std::optional<std::uint64_t> u64(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 8) return std::nullopt;
    std::uint64_t value{};
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + i])) << (i * 8);
    }
    return value;
}

std::string section_name(std::span<const std::byte> bytes, std::size_t offset) {
    std::array<char, 9> name{};
    for (std::size_t i = 0; i < 8 && offset + i < bytes.size(); ++i) {
        name[i] = static_cast<char>(std::to_integer<unsigned char>(bytes[offset + i]));
        if (name[i] == '\0') break;
    }
    return std::string{name.data()};
}

} // namespace

bool PeSection::executable() const noexcept { return (characteristics & section_mem_execute) != 0; }
bool PeSection::readable() const noexcept { return (characteristics & section_mem_read) != 0; }
bool PeSection::writable() const noexcept { return (characteristics & section_mem_write) != 0; }

std::optional<PeImage> PeImage::parse(std::span<const std::byte> bytes) {
    if (bytes.size() < 0x40 ||
        std::to_integer<unsigned char>(bytes[0]) != 'M' ||
        std::to_integer<unsigned char>(bytes[1]) != 'Z') {
        return std::nullopt;
    }

    const auto pe_offset_value = u32(bytes, 0x3c);
    if (!pe_offset_value) return std::nullopt;
    const auto pe_offset = static_cast<std::size_t>(*pe_offset_value);
    if (pe_offset > bytes.size() || bytes.size() - pe_offset < 24) return std::nullopt;

    if (std::to_integer<unsigned char>(bytes[pe_offset]) != 'P' ||
        std::to_integer<unsigned char>(bytes[pe_offset + 1]) != 'E' ||
        bytes[pe_offset + 2] != std::byte{0} || bytes[pe_offset + 3] != std::byte{0}) {
        return std::nullopt;
    }

    const auto coff = pe_offset + 4;
    const auto machine = u16(bytes, coff);
    const auto section_count = u16(bytes, coff + 2);
    const auto timestamp = u32(bytes, coff + 4);
    const auto optional_size = u16(bytes, coff + 16);
    if (!machine || !section_count || !timestamp || !optional_size || *section_count == 0) return std::nullopt;

    const auto optional = coff + 20;
    if (optional > bytes.size() || bytes.size() - optional < *optional_size || *optional_size < 60) return std::nullopt;

    const auto magic = u16(bytes, optional);
    if (!magic || (*magic != 0x10bu && *magic != 0x20bu)) return std::nullopt;
    const bool pe32_plus = *magic == 0x20bu;

    const auto entry_rva = u32(bytes, optional + 16);
    const auto image_size = u32(bytes, optional + 56);
    if (!entry_rva || !image_size) return std::nullopt;

    std::uint64_t image_base{};
    if (pe32_plus) {
        const auto value = u64(bytes, optional + 24);
        if (!value) return std::nullopt;
        image_base = *value;
    } else {
        const auto value = u32(bytes, optional + 28);
        if (!value) return std::nullopt;
        image_base = *value;
    }

    const auto section_table = optional + *optional_size;
    const auto section_bytes_required = static_cast<std::size_t>(*section_count) * 40u;
    if (section_table > bytes.size() || section_bytes_required > bytes.size() - section_table) return std::nullopt;

    PeImage image;
    image.machine_ = *machine;
    image.timestamp_ = *timestamp;
    image.image_base_ = image_base;
    image.entry_rva_ = *entry_rva;
    image.image_size_ = *image_size;
    image.pe32_plus_ = pe32_plus;
    image.sections_.reserve(*section_count);

    for (std::size_t i = 0; i < *section_count; ++i) {
        const auto offset = section_table + i * 40u;
        const auto virtual_size = u32(bytes, offset + 8);
        const auto virtual_address = u32(bytes, offset + 12);
        const auto raw_size = u32(bytes, offset + 16);
        const auto raw_offset = u32(bytes, offset + 20);
        const auto characteristics = u32(bytes, offset + 36);
        if (!virtual_size || !virtual_address || !raw_size || !raw_offset || !characteristics) return std::nullopt;
        if (*raw_size > 0 && (static_cast<std::size_t>(*raw_offset) > bytes.size() ||
                              static_cast<std::size_t>(*raw_size) > bytes.size() - static_cast<std::size_t>(*raw_offset))) {
            return std::nullopt;
        }

        image.sections_.push_back({
            section_name(bytes, offset),
            *virtual_size,
            *virtual_address,
            *raw_size,
            *raw_offset,
            *characteristics,
        });
    }

    return image;
}

std::uint16_t PeImage::machine() const noexcept { return machine_; }
std::uint32_t PeImage::timestamp() const noexcept { return timestamp_; }
std::uint64_t PeImage::image_base() const noexcept { return image_base_; }
std::uint32_t PeImage::entry_rva() const noexcept { return entry_rva_; }
std::uint32_t PeImage::image_size() const noexcept { return image_size_; }
bool PeImage::pe32_plus() const noexcept { return pe32_plus_; }
bool PeImage::x64() const noexcept { return machine_ == 0x8664u && pe32_plus_; }
const std::vector<PeSection>& PeImage::sections() const noexcept { return sections_; }

const PeSection* PeImage::find_section(std::string_view name) const noexcept {
    const auto found = std::ranges::find(sections_, name, &PeSection::name);
    return found == sections_.end() ? nullptr : &*found;
}

std::optional<std::span<const std::byte>> PeImage::section_bytes(
    std::span<const std::byte> image,
    std::string_view name) const noexcept {
    const auto* section = find_section(name);
    if (!section) return std::nullopt;
    const auto offset = static_cast<std::size_t>(section->raw_offset);
    const auto size = static_cast<std::size_t>(section->raw_size);
    if (offset > image.size() || size > image.size() - offset) return std::nullopt;
    return image.subspan(offset, size);
}

std::optional<std::size_t> PeImage::rva_to_file_offset(std::uint32_t rva) const noexcept {
    for (const auto& section : sections_) {
        if (rva < section.virtual_address) continue;
        const auto delta = static_cast<std::uint64_t>(rva) - section.virtual_address;
        if (delta >= section.raw_size) continue;
        const auto offset = static_cast<std::uint64_t>(section.raw_offset) + delta;
        if (offset > std::numeric_limits<std::size_t>::max()) return std::nullopt;
        return static_cast<std::size_t>(offset);
    }
    return std::nullopt;
}

std::optional<std::uint32_t> PeImage::file_offset_to_rva(std::size_t offset) const noexcept {
    for (const auto& section : sections_) {
        const auto raw_start = static_cast<std::uint64_t>(section.raw_offset);
        const auto raw_end = raw_start + section.raw_size;
        const auto file_offset = static_cast<std::uint64_t>(offset);
        if (file_offset < raw_start || file_offset >= raw_end) continue;
        const auto rva = static_cast<std::uint64_t>(section.virtual_address) + (file_offset - raw_start);
        if (rva > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
        return static_cast<std::uint32_t>(rva);
    }
    return std::nullopt;
}

} // namespace enrage::platform
