#include "core/build_catalog.hpp"

#include <algorithm>
#include <utility>

namespace enrage::core {

bool BuildCatalog::add(BuildProfile profile) {
    if (profile.id.empty()) return false;

    const auto duplicate = std::ranges::find_if(profiles_, [&](const BuildProfile& existing) {
        return existing.id == profile.id || existing.fingerprint == profile.fingerprint;
    });
    if (duplicate != profiles_.end()) return false;

    profiles_.push_back(std::move(profile));
    return true;
}

const BuildProfile* BuildCatalog::find(const platform::FileFingerprint& fingerprint) const noexcept {
    const auto found = std::ranges::find(profiles_, fingerprint, &BuildProfile::fingerprint);
    return found == profiles_.end() ? nullptr : &*found;
}

std::optional<platform::signature::Resolver> BuildCatalog::resolver_for(
    const platform::FileFingerprint& fingerprint) const {
    const auto* profile = find(fingerprint);
    if (!profile) return std::nullopt;

    platform::signature::Resolver resolver;
    for (const auto& symbol : profile->symbols) {
        if (!resolver.add(symbol)) return std::nullopt;
    }
    return resolver;
}

std::size_t BuildCatalog::size() const noexcept {
    return profiles_.size();
}

} // namespace enrage::core
