#pragma once

#include "platform/fingerprint.hpp"
#include "platform/pe_image.hpp"
#include "platform/scan_image.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace enrage::adapters {

class ImageProvider {
public:
    virtual ~ImageProvider() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual std::optional<platform::FileFingerprint> identity() const = 0;
    [[nodiscard]] virtual std::optional<platform::ScanImage> scan_image() const = 0;
};

class FileImageProvider final : public ImageProvider {
public:
    explicit FileImageProvider(std::filesystem::path path);

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] std::optional<platform::FileFingerprint> identity() const override;
    [[nodiscard]] std::optional<platform::ScanImage> scan_image() const override;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    std::filesystem::path path_;
};

// Represents an already-captured loaded image. The exact file fingerprint is
// supplied independently because relocated memory is not a stable build ID.
class LoadedSnapshotImageProvider final : public ImageProvider {
public:
    LoadedSnapshotImageProvider(
        platform::FileFingerprint identity,
        std::vector<std::byte> bytes,
        platform::PeImage metadata,
        std::uint64_t loaded_base);

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] std::optional<platform::FileFingerprint> identity() const override;
    [[nodiscard]] std::optional<platform::ScanImage> scan_image() const override;

private:
    platform::FileFingerprint identity_;
    std::vector<std::byte> bytes_;
    platform::PeImage metadata_;
    std::uint64_t loaded_base_{};
};

} // namespace enrage::adapters
