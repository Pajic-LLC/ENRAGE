#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace enrage::platform {

struct FileFingerprint {
    std::array<std::uint8_t, 32> sha256{};
    std::uint64_t size{};

    [[nodiscard]] std::string hex() const;
    [[nodiscard]] bool operator==(const FileFingerprint&) const = default;
};

[[nodiscard]] std::optional<FileFingerprint> fingerprint_file(const std::filesystem::path& path);

} // namespace enrage::platform
