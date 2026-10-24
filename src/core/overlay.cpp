#include "core/overlay.hpp"

#include <algorithm>
#include <map>
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

    std::error_code error;
    auto canonical_root = std::filesystem::weakly_canonical(mount.root, error);
    if (error || !std::filesystem::is_directory(canonical_root, error) || error) {
        return false;
    }
    mount.root = std::move(canonical_root);

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
        std::error_code error;
        const auto candidate = std::filesystem::weakly_canonical(mount.root / *normalized, error);
        if (error || !contained_by(mount.root, candidate)) {
            continue;
        }
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate;
        }
    }

    return std::nullopt;
}

std::vector<OverlayConflict> Overlay::conflicts() const {
    std::map<std::string, std::vector<std::string>> owners;

    for (const auto& mount : mounts_) {
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(mount.root, error), end;
             !error && it != end;
             it.increment(error)) {
            if (!it->is_regular_file(error) || error) {
                error.clear();
                continue;
            }

            const auto canonical = std::filesystem::weakly_canonical(it->path(), error);
            if (error || !contained_by(mount.root, canonical)) {
                error.clear();
                continue;
            }

            const auto logical = std::filesystem::relative(it->path(), mount.root, error);
            if (error) {
                error.clear();
                continue;
            }
            const auto normalized = normalize(logical);
            if (!normalized) continue;
            owners[normalized->generic_string()].push_back(mount.id);
        }
    }

    std::vector<OverlayConflict> result;
    for (auto& [path, mounts] : owners) {
        if (mounts.size() > 1) {
            result.push_back({std::filesystem::path(path), std::move(mounts)});
        }
    }
    return result;
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

bool Overlay::contained_by(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate) noexcept {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *root_it != *candidate_it) {
            return false;
        }
    }
    return true;
}

void Overlay::sort_mounts() {
    std::ranges::stable_sort(mounts_, [](const OverlayMount& left, const OverlayMount& right) {
        return left.priority > right.priority;
    });
}

} // namespace enrage::core
