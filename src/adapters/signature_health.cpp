#include "adapters/signature_health.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace enrage::adapters {
namespace {

AnchorHealth health_from_resolution(
    std::string_view anchor,
    std::string_view symbol,
    const platform::ImageResolution& resolution) {
    AnchorHealth health;
    health.name = std::string{anchor};

    switch (resolution.state) {
        case platform::signature::ResolutionState::unique:
            if (resolution.candidate_index && *resolution.candidate_index > 0) {
                health.state = AnchorHealthState::degraded;
                health.reason = "symbol " + std::string{symbol} +
                    " resolved using fallback candidate #" +
                    std::to_string(*resolution.candidate_index + 1);
            } else {
                health.state = AnchorHealthState::resolved;
                health.reason = "symbol " + std::string{symbol} + " resolved using primary signature";
            }
            break;
        case platform::signature::ResolutionState::ambiguous: {
            health.state = AnchorHealthState::missing;
            std::ostringstream message;
            message << "symbol " << symbol << " is ambiguous ("
                    << resolution.match_count << " matches)";
            health.reason = message.str();
            break;
        }
        case platform::signature::ResolutionState::not_found:
            health.state = AnchorHealthState::missing;
            health.reason = "symbol " + std::string{symbol} + " signature not found";
            break;
        case platform::signature::ResolutionState::unknown_symbol:
            health.state = AnchorHealthState::unresolved;
            health.reason = "symbol " + std::string{symbol} + " is not defined in the active profile";
            break;
    }

    return health;
}

} // namespace

bool SignatureHealthResolver::bind(std::string anchor, std::string symbol) {
    if (anchor.empty() || symbol.empty()) return false;
    if (std::ranges::find(bindings_, anchor, &SignatureAnchorBinding::anchor) != bindings_.end()) {
        return false;
    }
    bindings_.push_back({std::move(anchor), std::move(symbol)});
    return true;
}

std::vector<SignatureAnchorEvaluation> SignatureHealthResolver::evaluate(
    const platform::signature::Resolver& resolver,
    const platform::ScanImage& image) const {
    std::vector<SignatureAnchorEvaluation> results;
    results.reserve(bindings_.size());

    for (const auto& binding : bindings_) {
        auto resolution = platform::resolve_in_image(resolver, binding.symbol, image);
        results.push_back({
            health_from_resolution(binding.anchor, binding.symbol, resolution),
            std::move(resolution),
        });
    }
    return results;
}

bool SignatureHealthResolver::apply(
    CapabilityMap& capabilities,
    const platform::signature::Resolver& resolver,
    const platform::ScanImage& image) const {
    bool complete = true;
    for (const auto& evaluation : evaluate(resolver, image)) {
        if (!capabilities.set_anchor_state(
                evaluation.health.name,
                evaluation.health.state,
                evaluation.health.reason)) {
            complete = false;
        }
    }
    return complete;
}

const std::vector<SignatureAnchorBinding>& SignatureHealthResolver::bindings() const noexcept {
    return bindings_;
}

} // namespace enrage::adapters
