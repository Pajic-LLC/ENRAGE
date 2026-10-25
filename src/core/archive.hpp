#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace enrage::core {

struct ArchiveEntry {
    std::string path;
    std::uint64_t offset{};
    std::uint64_t size{};
};

class Archive {
public:
    static constexpr std::uint32_t format_version = 1;

    [[nodiscard]] static std::optional<Archive> open(const std::filesystem::path& path);
    [[nodiscard]] static bool create(
        const std::filesystem::path& source_directory,
        const std::filesystem::path& output_path);

    [[nodiscard]] const std::vector<ArchiveEntry>& entries() const noexcept;
    [[nodiscard]] bool contains(std::string_view logical_path) const noexcept;
    [[nodiscard]] bool contains_prefix(std::string_view logical_prefix) const noexcept;
    [[nodiscard]] std::optional<std::vector<std::byte>> read(std::string_view logical_path) const;
    [[nodiscard]] std::optional<std::string> read_text(std::string_view logical_path) const;
    [[nodiscard]] bool extract(
        std::string_view logical_path,
        const std::filesystem::path& destination_file) const;
    [[nodiscard]] bool extract_all(const std::filesystem::path& destination) const;

private:
    std::filesystem::path path_;
    std::vector<ArchiveEntry> entries_;
};

} // namespace enrage::core
