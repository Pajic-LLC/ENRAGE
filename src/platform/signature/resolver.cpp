#include "platform/signature/resolver.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <utility>

namespace enrage::platform::signature {

bool Resolver::add(SymbolSpec spec) {
    if (spec.name.empty() || spec.candidates.empty()) {
        return false;
    }

    const auto duplicate = std::ranges::find(specs_, spec.name, &SymbolSpec::name);
    if (duplicate != specs_.end()) {
        return false;
    }

    specs_.push_back(std::move(spec));
    return true;
}

SymbolResolution Resolver::resolve(
    std::string_view name,
    std::span<const std::byte> bytes) const {
    const auto* found = spec(name);
    if (!found) {
        return {ResolutionState::unknown_symbol, std::nullopt, 0, std::nullopt};
    }

    std::map<std::size_t, std::size_t> resolved_offsets;

    for (std::size_t candidate_index = 0; candidate_index < found->candidates.size(); ++candidate_index) {
        const auto& candidate = found->candidates[candidate_index];
        for (const auto match : candidate.pattern.find_all(bytes)) {
            std::optional<std::size_t> resolved;

            if (candidate.adjustment < 0) {
                auto magnitude = static_cast<std::size_t>(-(candidate.adjustment + 1));
                ++magnitude;
                if (magnitude <= match) {
                    resolved = match - magnitude;
                }
            } else {
                const auto adjustment = static_cast<std::size_t>(candidate.adjustment);
                if (adjustment <= std::numeric_limits<std::size_t>::max() - match) {
                    const auto adjusted = match + adjustment;
                    if (adjusted < bytes.size()) {
                        resolved = adjusted;
                    }
                }
            }

            if (!resolved) continue;
            const auto [it, inserted] = resolved_offsets.try_emplace(*resolved, candidate_index);
            if (!inserted && candidate_index < it->second) {
                it->second = candidate_index;
            }
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
