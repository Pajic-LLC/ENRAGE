#pragma once

#include "core/build_catalog.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enrage::core {

struct BuildProfileError {
    std::filesystem::path source;
    std::string message;
    std::size_t offset{};
};

struct BuildProfileParseResult {
    std::optional<BuildProfile> profile;
    std::optional<BuildProfileError> error;

    [[nodiscard]] bool ok() const noexcept { return profile.has_value(); }
};

struct BuildProfileImportResult {
    BuildCatalog catalog;
    std::vector<BuildProfileError> errors;
};

[[nodiscard]] BuildProfileParseResult parse_build_profile(
    std::string_view json,
    std::filesystem::path source = {});

[[nodiscard]] BuildProfileImportResult import_build_profiles(
    const std::filesystem::path& directory);

} // namespace enrage::core
