#pragma once

#include "adapters/capability_map.hpp"
#include "adapters/signature_health.hpp"
#include "core/build_catalog.hpp"
#include "platform/scan_image.hpp"
#include "platform/signature/resolver.hpp"

#include <cstdint>
#include <optional>

namespace enrage::adapters {

class ProfileRuntimeTopology {
public:
    [[nodiscard]] static std::optional<ProfileRuntimeTopology> from_profile(
        const core::BuildProfile& profile);

    bool evaluate(
        const platform::signature::Resolver& resolver,
        const platform::ScanImage& image);

    [[nodiscard]] std::uint64_t advertised_capabilities() const noexcept;
    [[nodiscard]] std::uint64_t capabilities() const noexcept;
    [[nodiscard]] AdapterHealthSnapshot health_snapshot() const;
    [[nodiscard]] const CapabilityMap& capability_map() const noexcept;
    [[nodiscard]] const SignatureHealthResolver& signature_health() const noexcept;

private:
    std::uint64_t advertised_capabilities_{};
    CapabilityMap capability_map_;
    SignatureHealthResolver signature_health_;
};

} // namespace enrage::adapters
