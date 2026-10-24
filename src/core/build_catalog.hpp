#pragma once

#include "platform/fingerprint.hpp"
#include "platform/signature/resolver.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace enrage::core {

struct BuildProfile {
    std::string id;
    platform::FileFingerprint fingerprint;
    std::uint64_t capabilities{};
    std::vector<platform::signature::SymbolSpec> symbols;
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
