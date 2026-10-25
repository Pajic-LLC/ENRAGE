#include "platform/scan_image.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace enrage::platform {

std::optional<ScanImage> ScanImage::from_file(std::span<const std::byte> bytes) {
    const auto metadata = PeImage::parse(bytes);
    if (!metadata) return std::nullopt;

    ScanImage image;
    image.layout_ = ImageLayout::file;
    image.base_address_ = metadata->image_base();
    image.bytes_.assign(bytes.begin(), bytes.end());
    image.regions_.reserve(metadata->sections().size());

    for (const auto& section : metadata->sections()) {
        const auto storage_offset = static_cast<std::size_t>(section.raw_offset);
        const auto size = static_cast<std::size_t>(section.raw_size);
        if (storage_offset > image.bytes_.size() || size > image.bytes_.size() - storage_offset) {
            return std::nullopt;
        }
        image.regions_.push_back({
            .name = section.name,
            .storage_offset = storage_offset,
            .size = size,
            .rva = section.virtual_address,
            .virtual_address = image.base_address_ + section.virtual_address,
            .characteristics = section.characteristics,
        });
    }
    return image;
}

std::optional<ScanImage> ScanImage::from_loaded_snapshot(
    std::span<const std::byte> bytes,
    const PeImage& metadata,
    std::uint64_t loaded_base) {
    if (bytes.empty()) return std::nullopt;

    ScanImage image;
    image.layout_ = ImageLayout::loaded;
    image.base_address_ = loaded_base;
    image.bytes_.assign(bytes.begin(), bytes.end());
    image.regions_.reserve(metadata.sections().size());

    for (const auto& section : metadata.sections()) {
        const auto storage_offset = static_cast<std::size_t>(section.virtual_address);
        if (storage_offset >= image.bytes_.size()) continue;
        const auto requested = static_cast<std::size_t>(
            section.virtual_size != 0 ? section.virtual_size : section.raw_size);
        const auto size = std::min(requested, image.bytes_.size() - storage_offset);
        if (size == 0) continue;
        image.regions_.push_back({
            .name = section.name,
            .storage_offset = storage_offset,
            .size = size,
            .rva = section.virtual_address,
            .virtual_address = loaded_base + section.virtual_address,
            .characteristics = section.characteristics,
        });
    }
    if (image.regions_.empty()) return std::nullopt;
    return image;
}

ImageLayout ScanImage::layout() const noexcept { return layout_; }
std::uint64_t ScanImage::base_address() const noexcept { return base_address_; }
const std::vector<ScanRegion>& ScanImage::regions() const noexcept { return regions_; }
std::span<const std::byte> ScanImage::bytes() const noexcept { return bytes_; }

std::optional<std::span<const std::byte>> ScanImage::region_bytes(std::string_view name) const noexcept {
    const auto* found = region(name);
    if (!found) return std::nullopt;
    if (found->storage_offset > bytes_.size() || found->size > bytes_.size() - found->storage_offset) {
        return std::nullopt;
    }
    return std::span<const std::byte>{bytes_}.subspan(found->storage_offset, found->size);
}

const ScanRegion* ScanImage::region(std::string_view name) const noexcept {
    const auto found = std::ranges::find(regions_, name, &ScanRegion::name);
    return found == regions_.end() ? nullptr : &*found;
}

const ScanRegion* ScanImage::region_for_storage_offset(std::size_t offset) const noexcept {
    const auto found = std::ranges::find_if(regions_, [&](const ScanRegion& region) {
        return offset >= region.storage_offset && offset - region.storage_offset < region.size;
    });
    return found == regions_.end() ? nullptr : &*found;
}

const ScanRegion* ScanImage::region_for_rva(std::uint32_t rva) const noexcept {
    const auto found = std::ranges::find_if(regions_, [&](const ScanRegion& region) {
        if (rva < region.rva) return false;
        return static_cast<std::uint64_t>(rva) - region.rva < region.size;
    });
    return found == regions_.end() ? nullptr : &*found;
}

std::optional<std::size_t> ScanImage::rva_to_storage_offset(std::uint32_t rva) const noexcept {
    const auto* found = region_for_rva(rva);
    if (!found) return std::nullopt;
    const auto delta = static_cast<std::size_t>(rva - found->rva);
    if (delta > std::numeric_limits<std::size_t>::max() - found->storage_offset) return std::nullopt;
    return found->storage_offset + delta;
}

std::optional<std::uint32_t> ScanImage::storage_offset_to_rva(std::size_t offset) const noexcept {
    const auto* found = region_for_storage_offset(offset);
    if (!found) return std::nullopt;
    const auto delta = offset - found->storage_offset;
    const auto rva = static_cast<std::uint64_t>(found->rva) + delta;
    if (rva > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    return static_cast<std::uint32_t>(rva);
}

std::optional<std::uint64_t> ScanImage::rva_to_virtual_address(std::uint32_t rva) const noexcept {
    if (!region_for_rva(rva)) return std::nullopt;
    if (rva > std::numeric_limits<std::uint64_t>::max() - base_address_) return std::nullopt;
    return base_address_ + rva;
}

ImageResolution resolve_in_image(
    const signature::Resolver& resolver,
    std::string_view symbol,
    const ScanImage& image,
    std::optional<std::string_view> requested_region) {
    const auto make_unique = [&](
        const ScanRegion& region,
        std::size_t local_offset,
        std::optional<std::size_t> candidate_index) {
        const auto storage_offset = region.storage_offset + local_offset;
        const auto rva64 = static_cast<std::uint64_t>(region.rva) + local_offset;
        if (rva64 > std::numeric_limits<std::uint32_t>::max()) {
            return ImageResolution{.state = signature::ResolutionState::not_found};
        }
        const auto rva = static_cast<std::uint32_t>(rva64);
        return ImageResolution{
            .state = signature::ResolutionState::unique,
            .match_count = 1,
            .storage_offset = storage_offset,
            .rva = rva,
            .virtual_address = image.base_address() + rva,
            .region = region.name,
            .candidate_index = candidate_index,
        };
    };

    auto effective_region = requested_region;
    if (!effective_region) {
        const auto* symbol_spec = resolver.spec(symbol);
        if (symbol_spec && !symbol_spec->region.empty()) effective_region = symbol_spec->region;
    }

    if (effective_region) {
        const auto* region = image.region(*effective_region);
        if (!region) {
            return {
                .state = resolver.spec(symbol)
                    ? signature::ResolutionState::not_found
                    : signature::ResolutionState::unknown_symbol,
            };
        }
        const auto bytes = image.region_bytes(*effective_region);
        if (!bytes) return {.state = signature::ResolutionState::not_found};
        const auto resolution = resolver.resolve(symbol, *bytes);
        if (resolution.state != signature::ResolutionState::unique || !resolution.offset) {
            return {
                .state = resolution.state,
                .match_count = resolution.match_count,
                .candidate_index = resolution.candidate_index,
            };
        }
        return make_unique(*region, *resolution.offset, resolution.candidate_index);
    }

    std::set<std::size_t> unique_storage_offsets;
    std::size_t ambiguous_matches{};
    bool known_symbol{};
    std::optional<std::size_t> unique_candidate_index;

    for (const auto& region : image.regions()) {
        const auto bytes = image.region_bytes(region.name);
        if (!bytes) continue;
        const auto resolution = resolver.resolve(symbol, *bytes);
        if (resolution.state != signature::ResolutionState::unknown_symbol) known_symbol = true;
        if (resolution.state == signature::ResolutionState::unique && resolution.offset) {
            unique_storage_offsets.insert(region.storage_offset + *resolution.offset);
            if (!unique_candidate_index || (resolution.candidate_index && *resolution.candidate_index < *unique_candidate_index)) {
                unique_candidate_index = resolution.candidate_index;
            }
        } else if (resolution.state == signature::ResolutionState::ambiguous) {
            ambiguous_matches += resolution.match_count;
        }
    }

    if (!known_symbol) return {};
    if (ambiguous_matches != 0 || unique_storage_offsets.size() > 1) {
        return {
            .state = signature::ResolutionState::ambiguous,
            .match_count = ambiguous_matches + unique_storage_offsets.size(),
        };
    }
    if (unique_storage_offsets.empty()) return {.state = signature::ResolutionState::not_found};

    const auto storage_offset = *unique_storage_offsets.begin();
    const auto* region = image.region_for_storage_offset(storage_offset);
    if (!region) return {.state = signature::ResolutionState::not_found};
    return make_unique(*region, storage_offset - region->storage_offset, unique_candidate_index);
}

} // namespace enrage::platform
