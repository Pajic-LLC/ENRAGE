#include "adapters/image_provider.hpp"

#include <fstream>
#include <limits>

namespace enrage::adapters {
namespace {

std::optional<std::vector<std::byte>> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return std::nullopt;

    const auto end = input.tellg();
    if (end < 0) return std::nullopt;
    const auto size = static_cast<std::uint64_t>(end);
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) return std::nullopt;
    }
    return bytes;
}

} // namespace

FileImageProvider::FileImageProvider(std::filesystem::path path)
    : path_(std::move(path)) {}

std::string_view FileImageProvider::name() const noexcept {
    return "file";
}

std::optional<platform::FileFingerprint> FileImageProvider::identity() const {
    return platform::fingerprint_file(path_);
}

std::optional<platform::ScanImage> FileImageProvider::scan_image() const {
    const auto bytes = read_bytes(path_);
    if (!bytes) return std::nullopt;
    return platform::ScanImage::from_file(*bytes);
}

const std::filesystem::path& FileImageProvider::path() const noexcept {
    return path_;
}

LoadedSnapshotImageProvider::LoadedSnapshotImageProvider(
    platform::FileFingerprint identity,
    std::vector<std::byte> bytes,
    platform::PeImage metadata,
    std::uint64_t loaded_base)
    : identity_(std::move(identity)),
      bytes_(std::move(bytes)),
      metadata_(std::move(metadata)),
      loaded_base_(loaded_base) {}

std::string_view LoadedSnapshotImageProvider::name() const noexcept {
    return "loaded-snapshot";
}

std::optional<platform::FileFingerprint> LoadedSnapshotImageProvider::identity() const {
    return identity_;
}

std::optional<platform::ScanImage> LoadedSnapshotImageProvider::scan_image() const {
    return platform::ScanImage::from_loaded_snapshot(bytes_, metadata_, loaded_base_);
}

} // namespace enrage::adapters
