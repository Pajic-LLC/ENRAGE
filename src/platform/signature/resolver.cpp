#include "platform/signature/resolver.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>

namespace enrage::platform::signature {
namespace {

std::optional<std::size_t> add_adjustment(std::size_t base, std::ptrdiff_t adjustment) {
    if (adjustment < 0) {
        auto magnitude = static_cast<std::size_t>(-(adjustment + 1));
        ++magnitude;
        if (magnitude > base) return std::nullopt;
        return base - magnitude;
    }
    const auto amount = static_cast<std::size_t>(adjustment);
    if (amount > std::numeric_limits<std::size_t>::max() - base) return std::nullopt;
    return base + amount;
}

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

std::optional<std::size_t> resolve_candidate(
    const SignatureCandidate& candidate,
    std::size_t match,
    std::span<const std::byte> bytes) {
    if (candidate.address_mode == AddressMode::direct) {
        const auto resolved = add_adjustment(match, candidate.adjustment);
        if (!resolved || *resolved >= bytes.size()) return std::nullopt;
        return resolved;
    }

    if (candidate.instruction_size == 0 ||
        candidate.displacement_offset > std::numeric_limits<std::size_t>::max() - match) {
        return std::nullopt;
    }
    const auto displacement_offset = match + candidate.displacement_offset;
    const auto displacement = read_i32_le(bytes, displacement_offset);
    if (!displacement || match > std::numeric_limits<std::size_t>::max() - candidate.instruction_size) {
        return std::nullopt;
    }

    const auto instruction_end = match + candidate.instruction_size;
    if (instruction_end > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;

    auto target = checked_add(
        static_cast<std::int64_t>(instruction_end),
        static_cast<std::int64_t>(*displacement));
    if (!target) return std::nullopt;
    target = checked_add(*target, static_cast<std::int64_t>(candidate.adjustment));
    if (!target || *target < 0) return std::nullopt;

    const auto unsigned_target = static_cast<std::uint64_t>(*target);
    if (unsigned_target >= bytes.size()) return std::nullopt;
    return static_cast<std::size_t>(unsigned_target);
}

} // namespace

bool Resolver::add(SymbolSpec spec) {
    if (spec.name.empty() || spec.candidates.empty()) return false;

    const auto duplicate = std::ranges::find(specs_, spec.name, &SymbolSpec::name);
    if (duplicate != specs_.end()) return false;

    for (const auto& candidate : spec.candidates) {
        if (candidate.address_mode == AddressMode::relative32) {
            if (candidate.instruction_size == 0 ||
                candidate.displacement_offset > candidate.instruction_size ||
                candidate.instruction_size - candidate.displacement_offset < 4) {
                return false;
            }
        }
    }

    specs_.push_back(std::move(spec));
    return true;
}

SymbolResolution Resolver::resolve(
    std::string_view name,
    std::span<const std::byte> bytes) const {
    const auto* found = spec(name);
    if (!found) return {ResolutionState::unknown_symbol, std::nullopt, 0, std::nullopt};

    std::map<std::size_t, std::size_t> resolved_offsets;

    for (std::size_t candidate_index = 0; candidate_index < found->candidates.size(); ++candidate_index) {
        const auto& candidate = found->candidates[candidate_index];
        for (const auto match : candidate.pattern.find_all(bytes)) {
            const auto resolved = resolve_candidate(candidate, match, bytes);
            if (!resolved) continue;
            const auto [it, inserted] = resolved_offsets.try_emplace(*resolved, candidate_index);
            if (!inserted && candidate_index < it->second) it->second = candidate_index;
        }
    }

    if (resolved_offsets.empty()) {
        return {ResolutionState::not_found, std::nullopt, 0, std::nullopt};
    }
    if (resolved_offsets.size() != 1) {
        return {ResolutionState::ambiguous, std::nullopt, resolved_offsets.size(), std::nullopt};
    }

    const auto& [offset, candidate_index] = *resolved_offsets.begin();
    return {ResolutionState::unique, offset, 1, candidate_index};
}

const SymbolSpec* Resolver::spec(std::string_view name) const noexcept {
    const auto found = std::ranges::find(specs_, name, &SymbolSpec::name);
    return found == specs_.end() ? nullptr : &*found;
}

std::size_t Resolver::symbol_count() const noexcept {
    return specs_.size();
}

} // namespace enrage::platform::signature
