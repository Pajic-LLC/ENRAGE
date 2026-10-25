#pragma once

#include "platform/pe_image.hpp"
#include "platform/signature/resolver.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace enrage::platform {

enum class ImageLayout {
    file,
    loaded,
};

struct ScanRegion {
    std::string name;
    std::size_t storage_offset{};
    std::size_t size{};
    std::uint32_t rva{};
    std::uint64_t virtual_address{};
    std::uint32_t characteristics{};
};

struct ImageResolution {
    signature::ResolutionState state{signature::ResolutionState::unknown_symbol};
    std::size_t match_count{};
    std::optional<std::size_t> storage_offset;
    std::optional<std::uint32_t> rva;
    std::optional<std::uint64_t> virtual_address;
    std::optional<std::string> region;
    std::optional<std::size_t> candidate_index;
};

class ScanImage {
public:
    [[nodiscard]] static std::optional<ScanImage> from_file(
        std::span<const std::byte> bytes);

    [[nodiscard]] static std::optional<ScanImage> from_loaded_snapshot(
        std::span<const std::byte> bytes,
        const PeImage& metadata,
        std::uint64_t loaded_base);

    [[nodiscard]] ImageLayout layout() const noexcept;
    [[nodiscard]] std::uint64_t base_address() const noexcept;
    [[nodiscard]] const std::vector<ScanRegion>& regions() const noexcept;
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] std::optional<std::span<const std::byte>> region_bytes(std::string_view name) const noexcept;
    [[nodiscard]] const ScanRegion* region(std::string_view name) const noexcept;
    [[nodiscard]] const ScanRegion* region_for_storage_offset(std::size_t offset) const noexcept;
    [[nodiscard]] const ScanRegion* region_for_rva(std::uint32_t rva) const noexcept;
    [[nodiscard]] std::optional<std::size_t> rva_to_storage_offset(std::uint32_t rva) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> storage_offset_to_rva(std::size_t offset) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> rva_to_virtual_address(std::uint32_t rva) const noexcept;

private:
    ImageLayout layout_{ImageLayout::file};
    std::uint64_t base_address_{};
    std::vector<std::byte> bytes_;
    std::vector<ScanRegion> regions_;
};

[[nodiscard]] ImageResolution resolve_in_image(
    const signature::Resolver& resolver,
    std::string_view symbol,
    const ScanImage& image,
    std::optional<std::string_view> region = std::nullopt);

// Resolve directly from bytes read from a file. Valid PE images use section/RVA
// semantics; non-PE research inputs fall back to generic storage offsets when
// the symbol has no section constraint.
[[nodiscard]] ImageResolution resolve_in_file_bytes(
    const signature::Resolver& resolver,
    std::string_view symbol,
    std::span<const std::byte> bytes);

} // namespace enrage::platform
