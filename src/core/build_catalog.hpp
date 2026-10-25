#pragma once

#include "platform/fingerprint.hpp"
#include "platform/signature/resolver.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace enrage::core {

struct ProfileAnchorBinding {
    std::string name;
    std::string symbol;
};

struct ProfileCapabilityRequirement {
    std::uint64_t capability{};
    std::vector<std::string> anchors;
};

struct BuildProfile {
    std::string id;
    platform::FileFingerprint fingerprint;
    std::uint64_t capabilities{};
    std::vector<platform::signature::SymbolSpec> symbols;
    std::vector<ProfileAnchorBinding> anchors;
    std::vector<ProfileCapabilityRequirement> requirements;
};

class BuildCatalog {
public:
    bool add(BuildProfile profile);

    [[nodiscard]] const BuildProfile* find(const platform::FileFingerprint& fingerprint) const noexcept;
    [[nodiscard]] std::optional<platform::signature::Resolver> resolver_for(
        const platform::FileFingerprint& fingerprint) const;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::vector<BuildProfile> profiles_;
};

} // namespace enrage::core
