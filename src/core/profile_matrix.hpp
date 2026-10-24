#pragma once

#include "platform/signature/resolver.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace enrage::core {

struct MatrixCell {
    std::filesystem::path image;
    bool expected{};
    platform::signature::ResolutionState state{platform::signature::ResolutionState::unknown_symbol};
    std::size_t match_count{};
    std::optional<std::size_t> offset;
};

struct MatrixRow {
    std::filesystem::path profile_source;
    std::string profile_id;
    std::string symbol;
    std::vector<MatrixCell> cells;
};

struct MatrixError {
    std::filesystem::path source;
    std::string message;
};

struct ProfileMatrixReport {
    std::size_t profiles_scanned{};
    std::size_t images_scanned{};
    std::vector<MatrixRow> rows;
    std::vector<MatrixError> errors;

    [[nodiscard]] bool ok() const noexcept;
};

[[nodiscard]] ProfileMatrixReport build_profile_matrix(
    const std::filesystem::path& profile_directory,
    const std::filesystem::path& image_directory);

[[nodiscard]] std::string profile_matrix_json(const ProfileMatrixReport& report);

} // namespace enrage::core
