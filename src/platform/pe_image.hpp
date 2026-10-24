#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace enrage::platform {

struct PeSection {
    std::string name;
    std::uint32_t virtual_size{};
    std::uint32_t virtual_address{};
    std::uint32_t raw_size{};
    std::uint32_t raw_offset{};
    std::uint32_t characteristics{};
};

class PeImage {
public:
    [[nodiscard]] static std::optional<PeImage> parse(std::span<const std::byte> bytes);

    [[nodiscard]] std::uint16_t machine() const noexcept;
    [[nodiscard]] std::uint32_t timestamp() const noexcept;
    [[nodiscard]] std::uint64_t image_base() const noexcept;
    [[nodiscard]] std::uint32_t entry_rva() const noexcept;
    [[nodiscard]] std::uint32_t image_size() const noexcept;
    [[nodiscard]] bool pe32_plus() const noexcept;
    [[nodiscard]] bool x64() const noexcept;
    [[nodiscard]] const std::vector<PeSection>& sections() const noexcept;
    [[nodiscard]] const PeSection* find_section(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<std::span<const std::byte>> section_bytes(
        std::span<const std::byte> image,
        std::string_view name) const noexcept;

private:
    std::uint16_t machine_{};
    std::uint32_t timestamp_{};
    std::uint64_t image_base_{};
    std::uint32_t entry_rva_{};
    std::uint32_t image_size_{};
    bool pe32_plus_{};
    std::vector<PeSection> sections_;
};

} // namespace enrage::platform
