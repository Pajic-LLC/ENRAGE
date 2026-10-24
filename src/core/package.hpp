#pragma once

#include "core/manifest.hpp"
#include "core/mod_registry.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace enrage::core {

enum class DiagnosticSeverity {
    info,
    warning,
    error,
};

enum class DiagnosticCode {
    manifest_missing,
    manifest_invalid,
    archive_invalid,
    archive_extract_failed,
    unsafe_path,
    missing_content_root,
    missing_plugin,
    dependency_error,
};

struct Diagnostic {
    DiagnosticSeverity severity{DiagnosticSeverity::error};
    DiagnosticCode code{};
    std::filesystem::path source;
    std::string mod_id;
    std::string message;
};

enum class PackageKind {
    directory,
    archive,
};

struct LoadedMod {
    PackageKind kind{PackageKind::directory};
    std::filesystem::path source;
    std::filesystem::path root;
    std::filesystem::path content_root;
    ModManifest manifest;
    std::vector<std::filesystem::path> native_plugins;
};

struct PackageLoadResult {
    std::vector<LoadedMod> mods;
    std::vector<Diagnostic> diagnostics;
};

class PackageLoader {
public:
    [[nodiscard]] static PackageLoadResult load(
        const ModRegistry& registry,
        const std::filesystem::path& state_root);
};

} // namespace enrage::core
