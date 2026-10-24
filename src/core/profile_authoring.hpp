#pragma once

#include "platform/fingerprint.hpp"
#include "platform/signature/resolver.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace enrage::core {

struct SymbolSeed {
    std::string name;
    std::size_t reference_offset{};
};

struct SignatureDraft {
    std::string name;
    std::size_t reference_offset{};
    std::size_t target_offset{};
    std::string pattern;
    std::ptrdiff_t adjustment{};
    double similarity{};
    std::size_t exact_bytes{};
    std::size_t wildcard_bytes{};
    std::size_t reference_match_count{};
    std::size_t target_match_count{};
    std::optional<std::string> section;
};

struct ProfileDraft {
    std::string id;
    platform::FileFingerprint fingerprint;
    std::uint64_t capabilities{};
    std::vector<SignatureDraft> symbols;
};

struct AuthoringError {
    std::string symbol;
    std::string message;
};

struct AuthoringResult {
    std::optional<ProfileDraft> profile;
    std::vector<AuthoringError> errors;

    [[nodiscard]] bool ok() const noexcept { return profile.has_value() && errors.empty(); }
};

struct AuthoringOptions {
    std::size_t minimum_radius{24};
    std::size_t maximum_radius{96};
    std::size_t seed_size{8};
    double minimum_similarity{0.55};
};

[[nodiscard]] AuthoringResult draft_profile(
    const std::filesystem::path& reference_image,
    const std::filesystem::path& target_image,
    std::string profile_id,
    const std::vector<SymbolSeed>& symbols,
    std::uint64_t capabilities = 0,
    AuthoringOptions options = {});

[[nodiscard]] std::string profile_draft_json(const ProfileDraft& profile);

struct ValidationSymbol {
    std::string name;
    platform::signature::ResolutionState state{platform::signature::ResolutionState::unknown_symbol};
    std::size_t match_count{};
    std::optional<std::size_t> offset;
};

struct ValidationBuild {
    std::filesystem::path profile_source;
    std::string profile_id;
    std::optional<std::filesystem::path> image;
    bool fingerprint_matched{};
    bool valid{};
    std::vector<ValidationSymbol> symbols;
};

struct ValidationError {
    std::filesystem::path source;
    std::string message;
};

struct ValidationReport {
    std::size_t profiles_scanned{};
    std::size_t images_scanned{};
    std::vector<ValidationBuild> builds;
    std::vector<ValidationError> errors;

    [[nodiscard]] bool ok() const noexcept;
};

[[nodiscard]] ValidationReport validate_profile_set(
    const std::filesystem::path& profile_directory,
    const std::filesystem::path& image_directory);

[[nodiscard]] std::string validation_report_json(const ValidationReport& report);

} // namespace enrage::core
