#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace enrage::adapters {

struct CapabilityAnchor {
    std::string name;
    bool available{};
};

struct CapabilityRequirement {
    std::uint64_t capability{};
    std::vector<std::string> anchors;
};

class CapabilityMap {
public:
    bool define_anchor(std::string name, bool available = true) {
        if (name.empty()) return false;
        if (std::ranges::find(anchors_, name, &CapabilityAnchor::name) != anchors_.end()) {
            return false;
        }
        anchors_.push_back({std::move(name), available});
        return true;
    }

    bool set_anchor(std::string_view name, bool available) noexcept {
        const auto found = std::ranges::find(anchors_, name, &CapabilityAnchor::name);
        if (found == anchors_.end()) return false;
        found->available = available;
        return true;
    }

    [[nodiscard]] bool anchor_available(std::string_view name) const noexcept {
        const auto found = std::ranges::find(anchors_, name, &CapabilityAnchor::name);
        return found != anchors_.end() && found->available;
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
