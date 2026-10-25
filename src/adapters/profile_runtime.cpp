#include "adapters/profile_runtime.hpp"

#include <algorithm>

namespace enrage::adapters {

std::optional<ProfileRuntimeTopology> ProfileRuntimeTopology::from_profile(
    const core::BuildProfile& profile) {
    ProfileRuntimeTopology topology;
    topology.advertised_capabilities_ = profile.capabilities;

    for (const auto& anchor : profile.anchors) {
        if (anchor.name.empty() || anchor.symbol.empty()) return std::nullopt;
        if (!std::ranges::any_of(profile.symbols, [&](const auto& symbol) {
                return symbol.name == anchor.symbol;
            })) {
            return std::nullopt;
        }
        if (!topology.capability_map_.define_anchor(
                anchor.name,
                AnchorHealthState::unresolved,
                "anchor has not been evaluated")) {
            return std::nullopt;
        }
        if (!topology.signature_health_.bind(anchor.name, anchor.symbol)) {
            return std::nullopt;
        }
    }

    for (const auto& requirement : profile.requirements) {
        if (requirement.capability == 0 ||
            (requirement.capability & (requirement.capability - 1)) != 0 ||
            (profile.capabilities & requirement.capability) == 0 ||
            requirement.anchors.empty()) {
            return std::nullopt;
        }
        for (const auto& anchor : requirement.anchors) {
            if (!topology.capability_map_.anchor_available(anchor) &&
                !std::ranges::any_of(profile.anchors, [&](const auto& declared) {
                    return declared.name == anchor;
                })) {
                return std::nullopt;
            }
        }
        if (!topology.capability_map_.require(requirement.capability, requirement.anchors)) {
            return std::nullopt;
        }
    }

    return topology;
}

bool ProfileRuntimeTopology::evaluate(
    const platform::signature::Resolver& resolver,
    const platform::ScanImage& image) {
    return signature_health_.apply(capability_map_, resolver, image);
}

std::uint64_t ProfileRuntimeTopology::advertised_capabilities() const noexcept {
    return advertised_capabilities_;
}

std::uint64_t ProfileRuntimeTopology::capabilities() const noexcept {
    return capability_map_.resolve(advertised_capabilities_);
}

AdapterHealthSnapshot ProfileRuntimeTopology::health_snapshot() const {
    return capability_map_.snapshot(advertised_capabilities_);
}

const CapabilityMap& ProfileRuntimeTopology::capability_map() const noexcept {
    return capability_map_;
}

const SignatureHealthResolver& ProfileRuntimeTopology::signature_health() const noexcept {
    return signature_health_;
}

} // namespace enrage::adapters
