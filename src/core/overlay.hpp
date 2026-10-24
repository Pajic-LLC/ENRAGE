#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enrage::core {

struct OverlayMount {
    std::string id;
    std::filesystem::path root;
    int priority{};
};

struct OverlayConflict {
    std::filesystem::path logical_path;
    std::vector<std::string> mounts;
};

class Overlay {
public:
    bool mount(OverlayMount mount);
    bool unmount(std::string_view id);
    void clear() noexcept;

    [[nodiscard]] std::optional<std::filesystem::path> resolve(
        const std::filesystem::path& logical_path) const;
    [[nodiscard]] std::vector<OverlayConflict> conflicts() const;
    [[nodiscard]] const std::vector<OverlayMount>& mounts() const noexcept;

private:
    static std::optional<std::filesystem::path> normalize(
        const std::filesystem::path& logical_path);
    static bool contained_by(
        const std::filesystem::path& root,
        const std::filesystem::path& candidate) noexcept;
    void sort_mounts();

    std::vector<OverlayMount> mounts_;
};

} // namespace enrage::core
