#pragma once

#include "core/profile_authoring.hpp"

#include <filesystem>
#include <string>

namespace enrage::core {

[[nodiscard]] AuthoringResult propagate_profile(
    const std::filesystem::path& reference_image,
    const std::filesystem::path& target_image,
    const std::filesystem::path& reference_profile,
    std::string target_profile_id,
    AuthoringOptions options = {});

} // namespace enrage::core
