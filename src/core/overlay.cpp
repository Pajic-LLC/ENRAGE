#include "core/overlay.hpp"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <limits>
#include <map>
#include <system_error>
#include <utility>

namespace enrage::core {
namespace {

bool component_equal(
    const std::filesystem::path& left,
    const std::filesystem::path& right) noexcept {
#if defined(_WIN32)
    const auto& a = left.native();
    const auto& b = right.native();
    if (a.size() != b.size()) return false;
    for (std::size_t index = 0; index < a.size(); ++index) {
        if (std::towlower(a[index]) != std::towlower(b[index])) return false;
    }
    return true;
#else
    return left == right;
#endif
}

std::string logical_key(const std::filesystem::path& path) {
    auto key = path.generic_string();
#if defined(_WIN32)
    std::ranges::transform(key, key.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
#endif
    return key;
}

} // namespace

bool Overlay::mount(OverlayMount mount) {
    if (mount.id.empty() || mount.root.empty()) return false;

    const auto duplicate = std::ranges::find(mounts_, mount.id, &OverlayMount::id);
    if (duplicate != mounts_.end()) return false;

    std::error_code error;
    auto canonical_root = std::filesystem::weakly_canonical(mount.root, error);
    if (error) return false;

    if (mount.kind == OverlayMountKind::directory) {
        if (!std::filesystem::is_directory(canonical_root, error) || error) return false;
        mount.root = std::move(canonical_root);
        mount.prefix.clear();
    } else {
        if (!std::filesystem::is_regular_file(canonical_root, error) || error) return false;
        const auto normalized_prefix = normalize(mount.prefix);
        if (!normalized_prefix) return false;
        auto archive = Archive::open(canonical_root);
        if (!archive) return false;
        if (!archive->contains_prefix(normalized_prefix->generic_string())) return false;

        mount.root = std::move(canonical_root);
        mount.prefix = *normalized_prefix;
        if (!archives_.emplace(mount.id, std::move(*archive)).second) return false;
    }

    mounts_.push_back(std::move(mount));
    sort_mounts();
    return true;
}

bool Overlay::mount_archive(
    std::string id,
    std::filesystem::path archive_path,
    std::filesystem::path prefix,
    int priority) {
    return mount({
        std::move(id),
        std::move(archive_path),
        priority,
        OverlayMountKind::archive,
        std::move(prefix),
    });
}

bool Overlay::unmount(std::string_view id) {
    const auto old_size = mounts_.size();
    std::erase_if(mounts_, [id](const OverlayMount& mount) {
        return mount.id == id;
    });
    archives_.erase(std::string{id});
    return mounts_.size() != old_size;
}

void Overlay::clear() noexcept {
    mounts_.clear();
    archives_.clear();
}

std::optional<std::filesystem::path> Overlay::resolve(
    const std::filesystem::path& logical_path) const {
    const auto normalized = normalize(logical_path);
    if (!normalized) return std::nullopt;

    for (const auto& mount : mounts_) {
        if (mount.kind == OverlayMountKind::archive) {
            const auto archive = archives_.find(mount.id);
            const auto entry = archive_entry_path(mount, *normalized);
            if (archive != archives_.end() && entry && archive->second.contains(*entry)) {
                return std::nullopt;
            }
            continue;
        }

        std::error_code error;
        const auto candidate = std::filesystem::weakly_canonical(mount.root / *normalized, error);
        if (error || !contained_by(mount.root, candidate)) continue;
        if (std::filesystem::is_regular_file(candidate, error) && !error) return candidate;
    }

    return std::nullopt;
}

std::optional<std::vector<std::byte>> Overlay::read(
    const std::filesystem::path& logical_path) const {
    const auto normalized = normalize(logical_path);
    if (!normalized) return std::nullopt;

    for (const auto& mount : mounts_) {
        if (mount.kind == OverlayMountKind::archive) {
            const auto archive = archives_.find(mount.id);
            const auto entry = archive_entry_path(mount, *normalized);
            if (archive == archives_.end() || !entry || !archive->second.contains(*entry)) continue;
            return archive->second.read(*entry);
        }

        std::error_code error;
        const auto candidate = std::filesystem::weakly_canonical(mount.root / *normalized, error);
        if (error || !contained_by(mount.root, candidate)) continue;
        if (!std::filesystem::is_regular_file(candidate, error) || error) continue;
        return read_file(candidate);
    }

    return std::nullopt;
}

std::optional<std::string> Overlay::read_text(
    const std::filesystem::path& logical_path) const {
    auto bytes = read(logical_path);
    if (!bytes) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

std::vector<OverlayEntry> Overlay::list(
    const std::filesystem::path& logical_directory) const {
    const auto normalized = normalize_directory(logical_directory);
    if (!normalized) return {};

    std::map<std::string, OverlayEntry, std::less<>> winners;

    for (const auto& mount : mounts_) {
        if (mount.kind == OverlayMountKind::archive) {
            const auto archive = archives_.find(mount.id);
            if (archive == archives_.end()) continue;

            std::filesystem::path full_prefix = mount.prefix;
            if (!normalized->empty()) full_prefix /= *normalized;
            auto prefix = full_prefix.generic_string();
            if (!prefix.empty() && prefix.back() != '/') prefix.push_back('/');

            for (const auto& entry : archive->second.entries()) {
                if (!entry.path.starts_with(prefix) || entry.path.size() <= prefix.size()) continue;
                const auto remainder = entry.path.substr(prefix.size());
                const auto slash = remainder.find('/');
                const auto child_text = slash == std::string::npos
                    ? remainder
                    : remainder.substr(0, slash);
                if (child_text.empty()) continue;
                const std::filesystem::path child{child_text};
                const auto key = logical_key(child);
                if (winners.contains(key)) continue;
                winners.emplace(key, OverlayEntry{
                    child,
                    slash == std::string::npos ? OverlayEntryKind::file : OverlayEntryKind::directory,
                    mount.id,
                });
            }
            continue;
        }

        std::error_code error;
        auto physical = mount.root;
        if (!normalized->empty()) physical /= *normalized;
        physical = std::filesystem::weakly_canonical(physical, error);
        if (error || !contained_by(mount.root, physical) ||
            !std::filesystem::is_directory(physical, error) || error) {
            continue;
        }

        for (std::filesystem::directory_iterator it(physical, error), end;
             !error && it != end;
             it.increment(error)) {
            const auto canonical = std::filesystem::weakly_canonical(it->path(), error);
            if (error || !contained_by(mount.root, canonical)) {
                error.clear();
                continue;
            }

            OverlayEntryKind kind;
            if (it->is_directory(error) && !error) kind = OverlayEntryKind::directory;
            else {
                error.clear();
                if (!it->is_regular_file(error) || error) {
                    error.clear();
                    continue;
                }
                kind = OverlayEntryKind::file;
            }

            const auto child = it->path().filename();
            const auto key = logical_key(child);
            if (winners.contains(key)) continue;
            winners.emplace(key, OverlayEntry{child, kind, mount.id});
        }
    }

    std::vector<OverlayEntry> result;
    result.reserve(winners.size());
    for (auto& [key, entry] : winners) {
        static_cast<void>(key);
        result.push_back(std::move(entry));
    }
    std::ranges::sort(result, [](const OverlayEntry& left, const OverlayEntry& right) {
        return logical_key(left.name) < logical_key(right.name);
    });
    return result;
}

std::vector<OverlayConflict> Overlay::conflicts() const {
    std::map<std::string, std::vector<std::string>> owners;

    for (const auto& mount : mounts_) {
        if (mount.kind == OverlayMountKind::archive) {
            const auto archive = archives_.find(mount.id);
            if (archive == archives_.end()) continue;

            auto prefix = mount.prefix.generic_string();
            if (!prefix.empty() && prefix.back() != '/') prefix.push_back('/');
            for (const auto& entry : archive->second.entries()) {
                if (!entry.path.starts_with(prefix) || entry.path.size() <= prefix.size()) continue;
                const auto logical = normalize(std::filesystem::path{entry.path.substr(prefix.size())});
                if (!logical) continue;
                owners[logical_key(*logical)].push_back(mount.id);
            }
            continue;
        }

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
            const auto normalized_path = normalize(logical);
            if (!normalized_path) continue;
            owners[logical_key(*normalized_path)].push_back(mount.id);
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
    const auto normalized = normalize_directory(logical_path);
    if (!normalized || normalized->empty()) return std::nullopt;
    return normalized;
}

std::optional<std::filesystem::path> Overlay::normalize_directory(
    const std::filesystem::path& logical_path) {
    if (logical_path.is_absolute() || logical_path.has_root_name() || logical_path.has_root_directory()) {
        return std::nullopt;
    }

    std::filesystem::path normalized;
    for (const auto& component : logical_path) {
        if (component == ".") continue;
        if (component == ".." || component.empty()) return std::nullopt;
        normalized /= component;
    }
    return normalized;
}

bool Overlay::contained_by(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate) noexcept {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || !component_equal(*root_it, *candidate_it)) return false;
    }
    return true;
}

std::optional<std::vector<std::byte>> Overlay::read_file(
    const std::filesystem::path& path) {
    std::error_code error;
    const auto raw_size = std::filesystem::file_size(path, error);
    if (error ||
        raw_size > std::numeric_limits<std::size_t>::max() ||
        raw_size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        return std::nullopt;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::vector<std::byte> bytes(static_cast<std::size_t>(raw_size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) return std::nullopt;
    }
    return bytes;
}

std::optional<std::string> Overlay::archive_entry_path(
    const OverlayMount& mount,
    const std::filesystem::path& normalized) const {
    if (mount.kind != OverlayMountKind::archive || mount.prefix.empty()) return std::nullopt;
    return (mount.prefix / normalized).generic_string();
}

void Overlay::sort_mounts() {
    std::ranges::stable_sort(mounts_, [](const OverlayMount& left, const OverlayMount& right) {
        return left.priority > right.priority;
    });
}

} // namespace enrage::core
