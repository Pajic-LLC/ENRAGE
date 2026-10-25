#include "core/archive.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>
#include <system_error>

namespace enrage::core {
namespace {

constexpr std::array<char, 4> magic{'E', 'N', 'R', '1'};
constexpr std::uint64_t header_size = 20;
constexpr std::uint64_t index_entry_fixed_size = 20;
std::atomic_uint64_t temporary_sequence{};

bool safe_logical_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const auto& part : path) {
        if (part == "." || part == ".." || part.empty()) return false;
    }
    return true;
}

std::string prefix_key(std::string_view prefix) {
    std::string key{prefix};
    while (!key.empty() && key.back() == '/') key.pop_back();
    if (!key.empty()) key.push_back('/');
    return key;
}

void write_u32(std::ostream& stream, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) stream.put(static_cast<char>((value >> (i * 8)) & 0xff));
}

void write_u64(std::ostream& stream, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) stream.put(static_cast<char>((value >> (i * 8)) & 0xff));
}

std::optional<std::uint32_t> read_u32(std::istream& stream) {
    std::uint32_t value{};
    for (int i = 0; i < 4; ++i) {
        const auto ch = stream.get();
        if (ch == std::char_traits<char>::eof()) return std::nullopt;
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(ch)) << (i * 8);
    }
    return value;
}

std::optional<std::uint64_t> read_u64(std::istream& stream) {
    std::uint64_t value{};
    for (int i = 0; i < 8; ++i) {
        const auto ch = stream.get();
        if (ch == std::char_traits<char>::eof()) return std::nullopt;
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(ch)) << (i * 8);
    }
    return value;
}

struct SourceEntry {
    std::filesystem::path source;
    std::string path;
    std::uint64_t size{};
};

std::filesystem::path temporary_sibling(
    const std::filesystem::path& destination,
    std::string_view role) {
    const auto seed = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto sequence = temporary_sequence.fetch_add(1, std::memory_order_relaxed);
    return destination.parent_path() /
           (destination.filename().string() + ".enrage-" + std::string{role} + "-" +
            std::to_string(seed ^ sequence));
}

class TemporaryPathGuard {
public:
    explicit TemporaryPathGuard(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~TemporaryPathGuard() {
        if (!active_) return;
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    void release() noexcept { active_ = false; }

private:
    std::filesystem::path path_;
    bool active_{true};
};

bool commit_temporary_file(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (!error) return true;

    // POSIX rename replaces an existing regular file atomically. Windows does
    // not, so preserve the previous destination through a same-directory
    // backup while committing the completed temporary file.
    error.clear();
    const auto status = std::filesystem::symlink_status(destination, error);
    if (error || !std::filesystem::is_regular_file(status)) return false;

    auto backup = temporary_sibling(destination, "backup");
    for (std::size_t attempt = 0; attempt < 16; ++attempt) {
        error.clear();
        if (!std::filesystem::exists(backup, error) && !error) break;
        backup = temporary_sibling(destination, "backup");
    }
    error.clear();
    if (std::filesystem::exists(backup, error) || error) return false;

    std::filesystem::rename(destination, backup, error);
    if (error) return false;

    error.clear();
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::error_code restore_error;
        std::filesystem::rename(backup, destination, restore_error);
        return false;
    }

    std::error_code ignored;
    std::filesystem::remove(backup, ignored);
    return true;
}

bool payloads_do_not_overlap(const std::vector<ArchiveEntry>& entries) {
    std::vector<const ArchiveEntry*> payloads;
    payloads.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.size != 0) payloads.push_back(&entry);
    }

    std::ranges::sort(payloads, [](const ArchiveEntry* left, const ArchiveEntry* right) {
        return left->offset < right->offset;
    });

    for (std::size_t index = 1; index < payloads.size(); ++index) {
        const auto* previous = payloads[index - 1];
        const auto* current = payloads[index];
        if (previous->size > current->offset - previous->offset) return false;
    }
    return true;
}

} // namespace

std::optional<Archive> Archive::open(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;

    std::array<char, 4> file_magic{};
    stream.read(file_magic.data(), static_cast<std::streamsize>(file_magic.size()));
    if (!stream || file_magic != magic) return std::nullopt;

    const auto version = read_u32(stream);
    const auto count = read_u32(stream);
    const auto index_bytes = read_u64(stream);
    if (!version || !count || !index_bytes || *version != format_version) return std::nullopt;

    std::error_code error;
    const auto raw_file_size = std::filesystem::file_size(path, error);
    if (error || raw_file_size > std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
    const auto file_size = static_cast<std::uint64_t>(raw_file_size);
    if (file_size < header_size || *index_bytes > file_size - header_size) return std::nullopt;

    if (*count != 0 && *count > *index_bytes / (index_entry_fixed_size + 1)) return std::nullopt;

    Archive archive;
    archive.path_ = path;
    archive.entries_.reserve(*count);

    std::uint64_t consumed{};
    for (std::uint32_t i = 0; i < *count; ++i) {
        const auto path_length = read_u32(stream);
        const auto offset = read_u64(stream);
        const auto size = read_u64(stream);
        if (!path_length || !offset || !size || *path_length == 0) return std::nullopt;

        if (consumed > *index_bytes ||
            *index_bytes - consumed < index_entry_fixed_size ||
            *path_length > *index_bytes - consumed - index_entry_fixed_size ||
            *path_length > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
            return std::nullopt;
        }

        std::string entry_path(*path_length, '\0');
        stream.read(entry_path.data(), static_cast<std::streamsize>(*path_length));
        if (!stream || !safe_logical_path(std::filesystem::path(entry_path))) return std::nullopt;

        const auto data_start = header_size + *index_bytes;
        if (*offset < data_start || *offset > file_size || *size > file_size - *offset) return std::nullopt;

        archive.entries_.push_back({std::move(entry_path), *offset, *size});
        consumed += index_entry_fixed_size + *path_length;
    }

    if (consumed != *index_bytes) return std::nullopt;

    std::ranges::sort(archive.entries_, {}, &ArchiveEntry::path);
    for (std::size_t i = 1; i < archive.entries_.size(); ++i) {
        if (archive.entries_[i - 1].path == archive.entries_[i].path) return std::nullopt;
    }
    if (!payloads_do_not_overlap(archive.entries_)) return std::nullopt;

    return archive;
}

bool Archive::create(const std::filesystem::path& source_directory, const std::filesystem::path& output_path) {
    std::error_code error;
    if (!std::filesystem::is_directory(source_directory, error) || error) return false;

    std::vector<SourceEntry> entries;
    for (std::filesystem::recursive_directory_iterator it(source_directory, error), end;
         !error && it != end;
         it.increment(error)) {
        const auto status = it->symlink_status(error);
        if (error) return false;
        if (std::filesystem::is_symlink(status)) return false;
        if (!std::filesystem::is_regular_file(status)) continue;

        const auto relative = std::filesystem::relative(it->path(), source_directory, error);
        if (error || !safe_logical_path(relative)) return false;
        const auto raw_size = it->file_size(error);
        if (error || raw_size > std::numeric_limits<std::uint64_t>::max()) return false;
        entries.push_back({it->path(), relative.generic_string(), static_cast<std::uint64_t>(raw_size)});
    }
    if (error || entries.empty() || entries.size() > std::numeric_limits<std::uint32_t>::max()) return false;

    std::ranges::sort(entries, {}, &SourceEntry::path);
    if (std::ranges::find(entries, std::string{"mod.json"}, &SourceEntry::path) == entries.end()) return false;

    std::uint64_t index_bytes{};
    for (const auto& entry : entries) {
        if (entry.path.empty() ||
            entry.path.size() > std::numeric_limits<std::uint32_t>::max() ||
            entry.path.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
            return false;
        }
        const auto record_size = index_entry_fixed_size + static_cast<std::uint64_t>(entry.path.size());
        if (index_bytes > std::numeric_limits<std::uint64_t>::max() - record_size) return false;
        index_bytes += record_size;
    }
    if (index_bytes > std::numeric_limits<std::uint64_t>::max() - header_size) return false;

    const auto output_parent = output_path.parent_path();
    if (!output_parent.empty()) {
        std::filesystem::create_directories(output_parent, error);
        if (error) return false;
    }

    const auto temporary = temporary_sibling(output_path, "pack");
    TemporaryPathGuard temporary_guard{temporary};
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return false;

    output.write(magic.data(), static_cast<std::streamsize>(magic.size()));
    write_u32(output, format_version);
    write_u32(output, static_cast<std::uint32_t>(entries.size()));
    write_u64(output, index_bytes);

    std::uint64_t next_offset = header_size + index_bytes;
    for (const auto& entry : entries) {
        if (entry.size > std::numeric_limits<std::uint64_t>::max() - next_offset) return false;
        write_u32(output, static_cast<std::uint32_t>(entry.path.size()));
        write_u64(output, next_offset);
        write_u64(output, entry.size);
        output.write(entry.path.data(), static_cast<std::streamsize>(entry.path.size()));
        if (!output) return false;
        next_offset += entry.size;
    }

    std::array<char, 64 * 1024> buffer{};
    for (const auto& entry : entries) {
        error.clear();
        const auto current_status = std::filesystem::symlink_status(entry.source, error);
        if (error || !std::filesystem::is_regular_file(current_status) ||
            std::filesystem::is_symlink(current_status)) {
            return false;
        }

        std::ifstream input(entry.source, std::ios::binary);
        if (!input) return false;

        std::uint64_t remaining = entry.size;
        while (remaining != 0) {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            input.read(buffer.data(), static_cast<std::streamsize>(chunk));
            if (input.gcount() != static_cast<std::streamsize>(chunk)) return false;
            output.write(buffer.data(), static_cast<std::streamsize>(chunk));
            if (!output) return false;
            remaining -= chunk;
        }

        // If a source grows after the index was sized, accepting the extra
        // bytes would shift every following payload away from its recorded
        // offset. Treat any size change as a failed transaction instead.
        if (input.get() != std::char_traits<char>::eof()) return false;
        if (!input.eof()) return false;
    }

    output.flush();
    if (!output) return false;
    output.close();
    if (!output) return false;

    if (!commit_temporary_file(temporary, output_path)) return false;
    temporary_guard.release();
    return true;
}

const std::vector<ArchiveEntry>& Archive::entries() const noexcept {
    return entries_;
}

bool Archive::contains(std::string_view logical_path) const noexcept {
    const auto it = std::ranges::lower_bound(entries_, logical_path, {}, &ArchiveEntry::path);
    return it != entries_.end() && it->path == logical_path;
}

bool Archive::contains_prefix(std::string_view logical_prefix) const noexcept {
    const auto key = prefix_key(logical_prefix);
    if (key.empty()) return false;
    const auto it = std::ranges::lower_bound(entries_, key, {}, &ArchiveEntry::path);
    return it != entries_.end() && it->path.starts_with(key);
}

std::optional<std::vector<std::byte>> Archive::read(std::string_view logical_path) const {
    const auto it = std::ranges::lower_bound(entries_, logical_path, {}, &ArchiveEntry::path);
    if (it == entries_.end() || it->path != logical_path ||
        it->size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        it->size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()) ||
        it->offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return std::nullopt;
    }

    std::ifstream stream(path_, std::ios::binary);
    if (!stream) return std::nullopt;
    stream.seekg(static_cast<std::streamoff>(it->offset));
    if (!stream) return std::nullopt;

    std::vector<std::byte> bytes(static_cast<std::size_t>(it->size));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream) return std::nullopt;
    }
    return bytes;
}

std::optional<std::string> Archive::read_text(std::string_view logical_path) const {
    auto bytes = read(logical_path);
    if (!bytes) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

bool Archive::extract(
    std::string_view logical_path,
    const std::filesystem::path& destination_file) const {
    if (!contains(logical_path)) return false;
    auto bytes = read(logical_path);
    if (!bytes) return false;

    std::error_code error;
    const auto parent = destination_file.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) return false;
    }

    std::ofstream output(destination_file, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    if (!bytes->empty()) {
        output.write(
            reinterpret_cast<const char*>(bytes->data()),
            static_cast<std::streamsize>(bytes->size()));
    }
    return static_cast<bool>(output);
}

bool Archive::extract_all(const std::filesystem::path& destination) const {
    std::error_code error;
    std::filesystem::create_directories(destination, error);
    if (error) return false;

    for (const auto& entry : entries_) {
        const auto relative = std::filesystem::path(entry.path);
        if (!safe_logical_path(relative)) return false;
        if (!extract(entry.path, destination / relative)) return false;
    }
    return true;
}

} // namespace enrage::core
