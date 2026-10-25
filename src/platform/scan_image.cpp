#include "platform/scan_image.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>

namespace enrage::platform {
namespace {

std::optional<std::int32_t> read_i32_le(
    std::span<const std::byte> bytes,
    std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) return std::nullopt;
    std::uint32_t raw{};
    for (std::size_t index = 0; index < 4; ++index) {
        raw |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8u);
    }
    return std::bit_cast<std::int32_t>(raw);
}

std::optional<std::int64_t> checked_add(std::int64_t left, std::int64_t right) {
    if (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) return std::nullopt;
    if (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right) return std::nullopt;
    return left + right;
}

std::optional<std::uint32_t> candidate_target_rva(
    const signature::SignatureCandidate& candidate,
    const ScanRegion& search_region,
    std::span<const std::byte> search_bytes,
    std::size_t match) {
    if (match > search_region.size) return std::nullopt;
    const auto match_rva = static_cast<std::uint64_t>(search_region.rva) + match;
    if (match_rva > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;

    std::int64_t target{};
    if (candidate.address_mode == signature::AddressMode::direct) {
        target = static_cast<std::int64_t>(match_rva);
    } else {
        if (candidate.instruction_size == 0 ||
            candidate.displacement_offset > std::numeric_limits<std::size_t>::max() - match) {
            return std::nullopt;
        }
        const auto displacement_position = match + candidate.displacement_offset;
        const auto displacement = read_i32_le(search_bytes, displacement_position);
        if (!displacement || match > std::numeric_limits<std::size_t>::max() - candidate.instruction_size) {
            return std::nullopt;
        }
        const auto instruction_end_local = match + candidate.instruction_size;
        if (instruction_end_local > search_region.size) return std::nullopt;
        const auto instruction_end_rva = static_cast<std::uint64_t>(search_region.rva) + instruction_end_local;
        if (instruction_end_rva > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        auto value = checked_add(
            static_cast<std::int64_t>(instruction_end_rva),
            static_cast<std::int64_t>(*displacement));
        if (!value) return std::nullopt;
        target = *value;
    }

    const auto adjusted = checked_add(target, static_cast<std::int64_t>(candidate.adjustment));
    if (!adjusted || *adjusted < 0 ||
        static_cast<std::uint64_t>(*adjusted) > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*adjusted);
}

} // namespace

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
        if (storage_offset > image.bytes_.size() || size > image.bytes_.size() - storage_offset) return std::nullopt;
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
    if (found->storage_offset > bytes_.size() || found->size > bytes_.size() - found->storage_offset) return std::nullopt;
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
    const auto* spec = resolver.spec(symbol);
    if (!spec) return {};

    auto effective_region = requested_region;
    if (!effective_region && !spec->region.empty()) effective_region = spec->region;

    std::vector<const ScanRegion*> search_regions;
    if (effective_region) {
        const auto* found = image.region(*effective_region);
        if (!found) return {.state = signature::ResolutionState::not_found};
        search_regions.push_back(found);
    } else {
        search_regions.reserve(image.regions().size());
        for (const auto& region : image.regions()) search_regions.push_back(&region);
    }

    struct Target {
        std::uint32_t rva{};
        std::size_t candidate_index{};
    };
    std::map<std::size_t, Target> targets;

    for (const auto* search_region : search_regions) {
        const auto search_bytes = image.region_bytes(search_region->name);
        if (!search_bytes) continue;

        for (std::size_t candidate_index = 0; candidate_index < spec->candidates.size(); ++candidate_index) {
            const auto& candidate = spec->candidates[candidate_index];
            for (const auto match : candidate.pattern.find_all(*search_bytes)) {
                const auto target_rva = candidate_target_rva(
                    candidate,
                    *search_region,
                    *search_bytes,
                    match);
                if (!target_rva) continue;
                const auto storage_offset = image.rva_to_storage_offset(*target_rva);
                if (!storage_offset) continue;

                const auto [it, inserted] = targets.try_emplace(
                    *storage_offset,
                    Target{*target_rva, candidate_index});
                if (!inserted && candidate_index < it->second.candidate_index) {
                    it->second.candidate_index = candidate_index;
                }
            }
        }
    }

    if (targets.empty()) return {.state = signature::ResolutionState::not_found};
    if (targets.size() != 1) {
        return {
            .state = signature::ResolutionState::ambiguous,
            .match_count = targets.size(),
        };
    }

    const auto& [storage_offset, target] = *targets.begin();
    const auto* target_region = image.region_for_rva(target.rva);
    const auto virtual_address = image.rva_to_virtual_address(target.rva);
    if (!target_region || !virtual_address) return {.state = signature::ResolutionState::not_found};

    return {
        .state = signature::ResolutionState::unique,
        .match_count = 1,
        .storage_offset = storage_offset,
        .rva = target.rva,
        .virtual_address = *virtual_address,
        .region = target_region->name,
        .candidate_index = target.candidate_index,
    };
}

ImageResolution resolve_in_file_bytes(
    const signature::Resolver& resolver,
    std::string_view symbol,
    std::span<const std::byte> bytes) {
    if (const auto image = ScanImage::from_file(bytes)) {
        return resolve_in_image(resolver, symbol, *image);
    }

    const auto* spec = resolver.spec(symbol);
    if (!spec) return {};
    if (!spec->region.empty()) {
        return {.state = signature::ResolutionState::not_found};
    }

    const auto raw = resolver.resolve(symbol, bytes);
    return {
        .state = raw.state,
        .match_count = raw.match_count,
        .storage_offset = raw.offset,
        .candidate_index = raw.candidate_index,
    };
}

} // namespace enrage::platform
