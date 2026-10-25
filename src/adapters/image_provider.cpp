#include "adapters/image_provider.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace enrage::adapters {
namespace {

std::optional<std::vector<std::byte>> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return std::nullopt;

    const auto end = input.tellg();
    if (end < 0) return std::nullopt;
    const auto size = static_cast<std::uint64_t>(end);
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
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

#if defined(_WIN32)
std::optional<std::filesystem::path> current_executable_path() {
    std::vector<wchar_t> buffer(32768);
    const auto length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return std::nullopt;
    return std::filesystem::path{std::wstring_view{buffer.data(), length}};
}

bool read_current_process_region(
    std::uintptr_t source,
    std::span<std::byte> destination) noexcept {
    if (destination.empty()) return true;

    SIZE_T bytes_read{};
    const BOOL ok = ReadProcessMemory(
        GetCurrentProcess(),
        reinterpret_cast<const void*>(source),
        destination.data(),
        destination.size(),
        &bytes_read);
    return ok != FALSE && bytes_read == destination.size();
}
#endif

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

CurrentProcessImageProvider::CurrentProcessImageProvider(
    std::filesystem::path executable_path,
    platform::FileFingerprint identity,
    std::vector<std::byte> loaded_bytes,
    platform::PeImage metadata,
    std::uint64_t loaded_base)
    : executable_path_(std::move(executable_path)),
      identity_(std::move(identity)),
      loaded_bytes_(std::move(loaded_bytes)),
      metadata_(std::move(metadata)),
      loaded_base_(loaded_base) {}

std::optional<CurrentProcessImageProvider> CurrentProcessImageProvider::create() {
#if defined(_WIN32)
    const auto path = current_executable_path();
    if (!path) return std::nullopt;

    const auto fingerprint = platform::fingerprint_file(*path);
    const auto file_bytes = read_bytes(*path);
    if (!fingerprint || !file_bytes) return std::nullopt;

    const auto metadata = platform::PeImage::parse(*file_bytes);
    if (!metadata || metadata->image_size() == 0) return std::nullopt;

    const auto module = GetModuleHandleW(nullptr);
    if (!module) return std::nullopt;
    const auto base = reinterpret_cast<std::uintptr_t>(module);

    std::vector<std::byte> loaded(metadata->image_size());

    // Capture headers first. A failed optional region read leaves zeroes behind;
    // signatures then fail closed rather than reading outside known mappings.
    const auto header_size = std::min<std::size_t>(loaded.size(), 0x1000u);
    if (header_size != 0) {
        (void)read_current_process_region(base, std::span<std::byte>{loaded.data(), header_size});
    }

    for (const auto& section : metadata->sections()) {
        const auto start = static_cast<std::size_t>(section.virtual_address);
        if (start >= loaded.size()) continue;

        const auto requested = static_cast<std::size_t>(
            std::max(section.virtual_size, section.raw_size));
        const auto size = std::min(requested, loaded.size() - start);
        if (size == 0) continue;

        (void)read_current_process_region(
            base + start,
            std::span<std::byte>{loaded.data() + start, size});
    }

    return CurrentProcessImageProvider{
        *path,
        *fingerprint,
        std::move(loaded),
        *metadata,
        static_cast<std::uint64_t>(base),
    };
#else
    return std::nullopt;
#endif
}

std::string_view CurrentProcessImageProvider::name() const noexcept {
    return "current-process";
}

std::optional<platform::FileFingerprint> CurrentProcessImageProvider::identity() const {
    return identity_;
}

std::optional<platform::ScanImage> CurrentProcessImageProvider::scan_image() const {
    return platform::ScanImage::from_loaded_snapshot(
        loaded_bytes_,
        metadata_,
        loaded_base_);
}

const std::filesystem::path& CurrentProcessImageProvider::executable_path() const noexcept {
    return executable_path_;
}

} // namespace enrage::adapters
