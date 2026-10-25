#include "core/package.hpp"

#include "core/archive.hpp"

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <system_error>

namespace enrage::core {
namespace {

std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) return std::nullopt;
    return buffer.str();
}

bool safe_relative(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const auto& part : path) {
        if (part == "." || part == ".." || part.empty()) return false;
    }
    return true;
}

std::string cache_key(const std::filesystem::path& path) {
    constexpr std::uint64_t offset_basis = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset_basis;
    const auto value = path.generic_string();
    for (const unsigned char ch : value) {
        hash ^= ch;
        hash *= prime;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

void push_manifest_error(
    PackageLoadResult& result,
    const std::filesystem::path& source,
    const ManifestParseResult& parsed) {
    const auto message = parsed.error
        ? parsed.error->message + " at byte " + std::to_string(parsed.error->offset)
        : std::string{"invalid manifest"};
    result.diagnostics.push_back({
        DiagnosticSeverity::error,
        DiagnosticCode::manifest_invalid,
        source,
        {},
        message,
    });
}

bool compatible_manifest(
    PackageLoadResult& result,
    const std::filesystem::path& source,
    const ModManifest& manifest) {
    if (manifest.manifest_version != manifest_format_version) {
        result.diagnostics.push_back({
            DiagnosticSeverity::error,
            DiagnosticCode::manifest_version_unsupported,
            source,
            manifest.id,
            "unsupported manifest version " + std::to_string(manifest.manifest_version),
        });
        return false;
    }

    if (manifest.runtime_abi != ENRAGE_ABI_VERSION) {
        result.diagnostics.push_back({
            DiagnosticSeverity::error,
            DiagnosticCode::runtime_abi_mismatch,
            source,
            manifest.id,
            "package requires runtime ABI " + std::to_string(manifest.runtime_abi) +
                ", host ABI is " + std::to_string(ENRAGE_ABI_VERSION),
        });
        return false;
    }

    return true;
}

std::optional<std::filesystem::path> validated_content_root(
    PackageLoadResult& result,
    const LoadedMod& mod) {
    const auto content_relative = std::filesystem::path(mod.manifest.content_root);
    if (!safe_relative(content_relative)) {
        result.diagnostics.push_back({
            DiagnosticSeverity::error,
            DiagnosticCode::unsafe_path,
            mod.source,
            mod.manifest.id,
            "contentRoot must be a safe relative path",
        });
        return std::nullopt;
    }
    return content_relative;
}

void finalize_directory_mod(PackageLoadResult& result, LoadedMod mod) {
    const auto content_relative = validated_content_root(result, mod);
    if (!content_relative) return;

    std::error_code error;
    const auto content = mod.root / *content_relative;
    if (std::filesystem::is_directory(content, error) && !error) {
        mod.content_root = content;
    } else {
        result.diagnostics.push_back({
            DiagnosticSeverity::warning,
            DiagnosticCode::missing_content_root,
            mod.source,
            mod.manifest.id,
            "content root is not present; this mod will not mount files",
        });
    }

    for (const auto& plugin_name : mod.manifest.native_plugins) {
        const auto relative = std::filesystem::path(plugin_name);
        if (!safe_relative(relative)) {
            result.diagnostics.push_back({
                DiagnosticSeverity::error,
                DiagnosticCode::unsafe_path,
                mod.source,
                mod.manifest.id,
                "native plugin path is not a safe relative path: " + plugin_name,
            });
            continue;
        }

        const auto plugin = mod.root / relative;
        error.clear();
        if (!std::filesystem::is_regular_file(plugin, error) || error) {
            result.diagnostics.push_back({
                DiagnosticSeverity::error,
                DiagnosticCode::missing_plugin,
                mod.source,
                mod.manifest.id,
                "native plugin is missing: " + plugin_name,
            });
            continue;
        }
        mod.native_plugins.push_back(plugin);
    }

    result.mods.push_back(std::move(mod));
}

void finalize_archive_mod(
    PackageLoadResult& result,
    LoadedMod mod,
    const Archive& archive,
    const std::filesystem::path& package_cache,
    const std::filesystem::path& stable_archive) {
    const auto content_relative = validated_content_root(result, mod);
    if (!content_relative) return;

    const auto content_prefix = content_relative->generic_string();
    if (archive.contains_prefix(content_prefix)) {
        mod.content_root = *content_relative;
    } else {
        result.diagnostics.push_back({
            DiagnosticSeverity::warning,
            DiagnosticCode::missing_content_root,
            mod.source,
            mod.manifest.id,
            "content root is not present; this mod will not mount files",
        });
    }

    for (const auto& plugin_name : mod.manifest.native_plugins) {
        const auto relative = std::filesystem::path(plugin_name);
        if (!safe_relative(relative)) {
            result.diagnostics.push_back({
                DiagnosticSeverity::error,
                DiagnosticCode::unsafe_path,
                mod.source,
                mod.manifest.id,
                "native plugin path is not a safe relative path: " + plugin_name,
            });
            continue;
        }

        if (!archive.contains(relative.generic_string())) {
            result.diagnostics.push_back({
                DiagnosticSeverity::error,
                DiagnosticCode::missing_plugin,
                mod.source,
                mod.manifest.id,
                "native plugin is missing: " + plugin_name,
            });
            continue;
        }

        const auto plugin = package_cache / relative;
        if (!archive.extract(relative.generic_string(), plugin)) {
            result.diagnostics.push_back({
                DiagnosticSeverity::error,
                DiagnosticCode::archive_extract_failed,
                mod.source,
                mod.manifest.id,
                "native plugin extraction failed: " + plugin_name,
            });
            continue;
        }
        mod.native_plugins.push_back(plugin);
    }

    mod.root = package_cache;
    mod.archive_path = stable_archive;
    result.mods.push_back(std::move(mod));
}

} // namespace

PackageLoadResult PackageLoader::load(
    const ModRegistry& registry,
    const std::filesystem::path& state_root) {
    PackageLoadResult result;

    std::error_code error;
    const auto cache_root = state_root / "cache";
    std::filesystem::create_directories(cache_root, error);
    const bool cache_ready = !error;
    if (!cache_ready) {
        result.diagnostics.push_back({
            DiagnosticSeverity::error,
            DiagnosticCode::archive_extract_failed,
            state_root,
            {},
            "could not create package cache directory",
        });
    }

    for (const auto& candidate : registry.candidates()) {
        if (!candidate.packaged) {
            const auto manifest_path = candidate.path / "mod.json";
            auto text = read_text_file(manifest_path);
            if (!text) {
                result.diagnostics.push_back({
                    DiagnosticSeverity::error,
                    DiagnosticCode::manifest_missing,
                    candidate.path,
                    candidate.id,
                    "mod.json is missing or unreadable",
                });
                continue;
            }

            auto parsed = parse_manifest(*text);
            if (!parsed.ok()) {
                push_manifest_error(result, candidate.path, parsed);
                continue;
            }
            if (!compatible_manifest(result, candidate.path, *parsed.manifest)) continue;

            LoadedMod mod;
            mod.kind = PackageKind::directory;
            mod.source = candidate.path;
            mod.root = candidate.path;
            mod.manifest = std::move(*parsed.manifest);
            finalize_directory_mod(result, std::move(mod));
            continue;
        }

        if (!cache_ready) continue;

        const auto package_cache = cache_root / cache_key(candidate.path);
        std::filesystem::remove_all(package_cache, error);
        error.clear();
        std::filesystem::create_directories(package_cache, error);
        if (error) {
            result.diagnostics.push_back({
                DiagnosticSeverity::error,
                DiagnosticCode::archive_extract_failed,
                candidate.path,
                candidate.id,
                "could not prepare package cache directory",
            });
            continue;
        }

        const auto stable_archive = package_cache / "package.enr";
        std::filesystem::copy_file(
            candidate.path,
            stable_archive,
            std::filesystem::copy_options::overwrite_existing,
            error);
        if (error) {
            result.diagnostics.push_back({
                DiagnosticSeverity::error,
                DiagnosticCode::archive_extract_failed,
                candidate.path,
                candidate.id,
                "could not materialize stable package container",
            });
            continue;
        }

        auto archive = Archive::open(stable_archive);
        if (!archive) {
            result.diagnostics.push_back({
                DiagnosticSeverity::error,
                DiagnosticCode::archive_invalid,
                candidate.path,
                candidate.id,
                "invalid ENR archive",
            });
            continue;
        }

        auto manifest_text = archive->read_text("mod.json");
        if (!manifest_text) {
            result.diagnostics.push_back({
                DiagnosticSeverity::error,
                DiagnosticCode::manifest_missing,
                candidate.path,
                candidate.id,
                "archive does not contain mod.json",
            });
            continue;
        }

        auto parsed = parse_manifest(*manifest_text);
        if (!parsed.ok()) {
            push_manifest_error(result, candidate.path, parsed);
            continue;
        }
        if (!compatible_manifest(result, candidate.path, *parsed.manifest)) continue;

        LoadedMod mod;
        mod.kind = PackageKind::archive;
        mod.source = candidate.path;
        mod.manifest = std::move(*parsed.manifest);
        finalize_archive_mod(
            result,
            std::move(mod),
            *archive,
            package_cache,
            stable_archive);
    }

    return result;
}

} // namespace enrage::core
