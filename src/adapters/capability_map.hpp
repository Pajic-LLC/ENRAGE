#pragma once

#include "adapters/adapter.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace enrage::adapters {

struct CapabilityAnchor {
    std::string name;
    AnchorHealthState state{AnchorHealthState::resolved};
    std::string reason;
};

struct CapabilityRequirement {
    std::uint64_t capability{};
    std::vector<std::string> anchors;
};

class CapabilityMap {
public:
    bool define_anchor(std::string name, bool available = true) {
        return define_anchor(
            std::move(name),
            available ? AnchorHealthState::resolved : AnchorHealthState::missing,
            available ? std::string{} : std::string{"anchor unavailable"});
    }

    bool define_anchor(std::string name, AnchorHealthState state, std::string reason = {}) {
        if (name.empty()) return false;
        if (std::ranges::find(anchors_, name, &CapabilityAnchor::name) != anchors_.end()) {
            return false;
        }
        anchors_.push_back({std::move(name), state, std::move(reason)});
        return true;
    }

    bool set_anchor(std::string_view name, bool available) noexcept {
        return set_anchor_state(
            name,
            available ? AnchorHealthState::resolved : AnchorHealthState::missing,
            available ? std::string{} : std::string{"anchor unavailable"});
    }

    bool set_anchor_state(
        std::string_view name,
        AnchorHealthState state,
        std::string reason = {}) noexcept {
        const auto found = std::ranges::find(anchors_, name, &CapabilityAnchor::name);
        if (found == anchors_.end()) return false;
        found->state = state;
        found->reason = std::move(reason);
        return true;
    }

    [[nodiscard]] bool anchor_available(std::string_view name) const noexcept {
        const auto found = std::ranges::find(anchors_, name, &CapabilityAnchor::name);
        if (found == anchors_.end()) return false;
        return found->state == AnchorHealthState::resolved ||
               found->state == AnchorHealthState::degraded;
    }

    bool require(std::uint64_t capability, std::vector<std::string> anchors) {
        if (capability == 0 || anchors.empty()) return false;
        if (std::ranges::find(requirements_, capability, &CapabilityRequirement::capability) != requirements_.end()) {
            return false;
        }
        if (std::ranges::any_of(anchors, [](const std::string& anchor) { return anchor.empty(); })) {
            return false;
        }
        std::ranges::sort(anchors);
        anchors.erase(std::unique(anchors.begin(), anchors.end()), anchors.end());
        requirements_.push_back({capability, std::move(anchors)});
        return true;
    }

    [[nodiscard]] std::uint64_t resolve(std::uint64_t advertised) const noexcept {
        auto available = advertised;
        for (const auto& requirement : requirements_) {
            if ((advertised & requirement.capability) == 0) continue;
            const bool satisfied = std::ranges::all_of(requirement.anchors, [&](const std::string& anchor) {
                return anchor_available(anchor);
            });
            if (!satisfied) available &= ~requirement.capability;
        }
        return available;
    }

    [[nodiscard]] AdapterHealthSnapshot snapshot(std::uint64_t advertised) const {
        AdapterHealthSnapshot snapshot;
        snapshot.anchors.reserve(anchors_.size());
        for (const auto& anchor : anchors_) {
            snapshot.anchors.push_back({anchor.name, anchor.state, anchor.reason});
        }

        snapshot.capabilities.reserve(requirements_.size());
        for (const auto& requirement : requirements_) {
            CapabilityHealth health;
            health.capability = requirement.capability;
            health.advertised = (advertised & requirement.capability) != 0;
            health.required_anchors = requirement.anchors;

            if (health.advertised) {
                for (const auto& name : requirement.anchors) {
                    const auto found = std::ranges::find(anchors_, name, &CapabilityAnchor::name);
                    if (found == anchors_.end() ||
                        found->state == AnchorHealthState::unresolved ||
                        found->state == AnchorHealthState::missing) {
                        health.blocking_anchors.push_back(name);
                    } else if (found->state == AnchorHealthState::degraded) {
                        health.degraded_anchors.push_back(name);
                    }
                }
            }

            health.available = health.advertised && health.blocking_anchors.empty();
            snapshot.capabilities.push_back(std::move(health));
        }
        return snapshot;
    }

    [[nodiscard]] const std::vector<CapabilityAnchor>& anchors() const noexcept {
        return anchors_;
    }

    [[nodiscard]] const std::vector<CapabilityRequirement>& requirements() const noexcept {
        return requirements_;
    }

private:
    std::vector<CapabilityAnchor> anchors_;
    std::vector<CapabilityRequirement> requirements_;
};

} // namespace enrage::adapters
