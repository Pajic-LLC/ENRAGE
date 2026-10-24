#pragma once

#include "platform/signature/resolver.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace enrage::core {

struct InspectionSection {
    std::string name;
    std::uint32_t virtual_size{};
    std::uint32_t virtual_address{};
    std::uint32_t raw_size{};
    std::uint32_t raw_offset{};
    std::uint32_t characteristics{};
};

struct InspectionSymbol {
    std::string name;
    platform::signature::ResolutionState state{platform::signature::ResolutionState::unknown_symbol};
    std::size_t match_count{};
    std::optional<std::size_t> file_offset;
    std::optional<std::uint64_t> virtual_address;
    std::optional<std::uint32_t> rva;
    std::optional<std::string> section;
};

struct InspectionProfileError {
    std::filesystem::path source;
    std::string message;
    std::size_t offset{};
};

struct InspectionReport {
    std::filesystem::path image;
    std::string sha256;
    std::uint64_t file_size{};

    bool pe_valid{};
    bool pe32_plus{};
    bool x64{};
    std::uint16_t machine{};
    std::uint32_t timestamp{};
    std::uint64_t image_base{};
    std::uint32_t entry_rva{};
    std::uint32_t image_size{};
    std::vector<InspectionSection> sections;

    std::size_t imported_profile_count{};
    std::vector<InspectionProfileError> profile_errors;
    std::optional<std::string> matched_profile;
    std::uint64_t capabilities{};
    std::vector<InspectionSymbol> symbols;
};

[[nodiscard]] std::optional<InspectionReport> inspect_executable(
    const std::filesystem::path& image,
    const std::optional<std::filesystem::path>& profile_directory = std::nullopt);

[[nodiscard]] std::string inspection_report_json(const InspectionReport& report);

} // namespace enrage::core
