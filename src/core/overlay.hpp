#pragma once

#include "core/archive.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enrage::core {

enum class OverlayMountKind {
    directory,
    archive,
};

enum class OverlayEntryKind {
    file,
    directory,
};

struct OverlayMount {
    std::string id;
    std::filesystem::path root;
    int priority{};
    OverlayMountKind kind{OverlayMountKind::directory};
    std::filesystem::path prefix;
};

struct OverlayEntry {
    std::filesystem::path name;
    OverlayEntryKind kind{OverlayEntryKind::file};
    std::string mount_id;
};

struct OverlayConflict {
    std::filesystem::path logical_path;
    std::vector<std::string> mounts;
};

class Overlay {
public:
    bool mount(OverlayMount mount);
    bool mount_archive(
        std::string id,
        std::filesystem::path archive_path,
        std::filesystem::path prefix,
        int priority);
    bool unmount(std::string_view id);
    void clear() noexcept;

    // Returns a physical path only when the winning mount is directory-backed.
    // Use read() for a backend-neutral lookup.
    [[nodiscard]] std::optional<std::filesystem::path> resolve(
        const std::filesystem::path& logical_path) const;
    [[nodiscard]] std::optional<std::vector<std::byte>> read(
        const std::filesystem::path& logical_path) const;
    [[nodiscard]] std::optional<std::string> read_text(
        const std::filesystem::path& logical_path) const;
    [[nodiscard]] std::vector<OverlayEntry> list(
        const std::filesystem::path& logical_directory = {}) const;
    [[nodiscard]] std::vector<OverlayConflict> conflicts() const;
    [[nodiscard]] const std::vector<OverlayMount>& mounts() const noexcept;

private:
    static std::optional<std::filesystem::path> normalize(
        const std::filesystem::path& logical_path);
    static std::optional<std::filesystem::path> normalize_directory(
        const std::filesystem::path& logical_path);
    static bool contained_by(
        const std::filesystem::path& root,
        const std::filesystem::path& candidate) noexcept;
    static std::optional<std::vector<std::byte>> read_file(
        const std::filesystem::path& path);
    [[nodiscard]] std::optional<std::string> archive_entry_path(
        const OverlayMount& mount,
        const std::filesystem::path& normalized) const;
    void sort_mounts();

    std::vector<OverlayMount> mounts_;
    std::map<std::string, Archive, std::less<>> archives_;
};

} // namespace enrage::core
