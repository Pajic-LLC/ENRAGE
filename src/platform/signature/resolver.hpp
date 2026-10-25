#pragma once

#include "platform/signature/pattern.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace enrage::platform::signature {

enum class ResolutionState {
    unknown_symbol,
    not_found,
    unique,
    ambiguous,
};

struct SignatureCandidate {
    Pattern pattern;
    std::ptrdiff_t adjustment{};
};

struct SymbolSpec {
    std::string name;
    std::vector<SignatureCandidate> candidates;
    std::string region;
};

struct SymbolResolution {
    ResolutionState state{ResolutionState::unknown_symbol};
    std::optional<std::size_t> offset;
    std::size_t match_count{};
    std::optional<std::size_t> candidate_index;
};

class Resolver {
public:
    bool add(SymbolSpec spec);

    [[nodiscard]] SymbolResolution resolve(
        std::string_view name,
        std::span<const std::byte> bytes) const;

    [[nodiscard]] const SymbolSpec* spec(std::string_view name) const noexcept;
    [[nodiscard]] std::size_t symbol_count() const noexcept;

private:
    std::vector<SymbolSpec> specs_;
};

} // namespace enrage::platform::signature
