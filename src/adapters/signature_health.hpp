#pragma once

#include "adapters/adapter.hpp"
#include "adapters/capability_map.hpp"
#include "platform/scan_image.hpp"
#include "platform/signature/resolver.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace enrage::adapters {

struct SignatureAnchorBinding {
    std::string anchor;
    std::string symbol;
};

struct SignatureAnchorEvaluation {
    AnchorHealth health;
    platform::ImageResolution resolution;
};

class SignatureHealthResolver {
public:
    bool bind(std::string anchor, std::string symbol);

    [[nodiscard]] std::vector<SignatureAnchorEvaluation> evaluate(
        const platform::signature::Resolver& resolver,
        const platform::ScanImage& image) const;

    bool apply(
        CapabilityMap& capabilities,
        const platform::signature::Resolver& resolver,
        const platform::ScanImage& image) const;

    [[nodiscard]] const std::vector<SignatureAnchorBinding>& bindings() const noexcept;

private:
    std::vector<SignatureAnchorBinding> bindings_;
};

} // namespace enrage::adapters
