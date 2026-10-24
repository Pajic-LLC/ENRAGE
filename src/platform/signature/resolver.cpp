#include "platform/signature/resolver.hpp"

#include <algorithm>
#include <limits>
#include <set>
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
        return {ResolutionState::unknown_symbol, std::nullopt, 0};
    }

    std::set<std::size_t> resolved_offsets;

    for (const auto& candidate : found->candidates) {
        for (const auto match : candidate.pattern.find_all(bytes)) {
            if (candidate.adjustment < 0) {
                const auto magnitude = static_cast<std::size_t>(-candidate.adjustment);
                if (magnitude > match) {
                    continue;
                }
                resolved_offsets.insert(match - magnitude);
                continue;
            }

            const auto adjustment = static_cast<std::size_t>(candidate.adjustment);
            if (adjustment > std::numeric_limits<std::size_t>::max() - match) {
                continue;
            }
            const auto resolved = match + adjustment;
            if (resolved < bytes.size()) {
                resolved_offsets.insert(resolved);
            }
        }
    }

    if (resolved_offsets.empty()) {
        return {ResolutionState::not_found, std::nullopt, 0};
    }

    if (resolved_offsets.size() != 1) {
        return {ResolutionState::ambiguous, std::nullopt, resolved_offsets.size()};
    }

    return {ResolutionState::unique, *resolved_offsets.begin(), 1};
}

const SymbolSpec* Resolver::spec(std::string_view name) const noexcept {
    const auto found = std::ranges::find(specs_, name, &SymbolSpec::name);
    return found == specs_.end() ? nullptr : &*found;
}

std::size_t Resolver::symbol_count() const noexcept {
    return specs_.size();
}

} // namespace enrage::platform::signature
