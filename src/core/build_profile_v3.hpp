#pragma once

#include "core/build_profile_io.hpp"

#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

namespace enrage::core {

[[nodiscard]] BuildProfileParseResult parse_build_profile_v3(
    std::string_view json,
    std::filesystem::path source = {});

[[nodiscard]] BuildProfileImportResult import_build_profiles_v3(
    const std::filesystem::path& directory);

[[nodiscard]] std::string serialize_build_profile_v3(const BuildProfile& profile);

} // namespace enrage::core
