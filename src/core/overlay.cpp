#include "core/overlay.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

namespace enrage::core {

bool Overlay::mount(OverlayMount mount) {
    if (mount.id.empty() || mount.root.empty()) {
        return false;
    }

    const auto duplicate = std::ranges::find(mounts_, mount.id, &OverlayMount::id);
    if (duplicate != mounts_.end()) {
        return false;
    }

    mounts_.push_back(std::move(mount));
    sort_mounts();
    return true;
}

bool Overlay::unmount(std::string_view id) {
    const auto old_size = mounts_.size();
    std::erase_if(mounts_, [id](const OverlayMount& mount) {
        return mount.id == id;
    });
    return mounts_.size() != old_size;
}

void Overlay::clear() noexcept {
    mounts_.clear();
}

std::optional<std::filesystem::path> Overlay::resolve(
    const std::filesystem::path& logical_path) const {
    const auto normalized = normalize(logical_path);
    if (!normalized) {
        return std::nullopt;
    }

    for (const auto& mount : mounts_) {
        const auto candidate = mount.root / *normalized;
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate;
        }
    }

    return std::nullopt;
}

const std::vector<OverlayMount>& Overlay::mounts() const noexcept {
    return mounts_;
}

std::optional<std::filesystem::path> Overlay::normalize(
    const std::filesystem::path& logical_path) {
    if (logical_path.empty() || logical_path.is_absolute() ||
        logical_path.has_root_name() || logical_path.has_root_directory()) {
        return std::nullopt;
    }

    std::filesystem::path normalized;
    for (const auto& component : logical_path) {
        if (component == ".") {
            continue;
        }
        if (component == "..") {
            return std::nullopt;
        }
        normalized /= component;
    }

    if (normalized.empty()) {
        return std::nullopt;
    }

    return normalized;
}

void Overlay::sort_mounts() {
    std::ranges::stable_sort(mounts_, [](const OverlayMount& left, const OverlayMount& right) {
        return left.priority > right.priority;
    });
}

} // namespace enrage::core
