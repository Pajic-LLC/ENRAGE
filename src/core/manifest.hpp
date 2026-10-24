#pragma once

#include "core/mod_graph.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enrage::core {

struct ModManifest {
    std::string id;
    std::string name;
    std::string version;
    int priority{};
    std::vector<std::string> dependencies;
    std::vector<std::string> load_after;
    std::vector<std::string> conflicts;
    std::string content_root{"files"};
    std::vector<std::string> native_plugins;

    [[nodiscard]] ModSpec to_spec() const;
};

struct ManifestError {
    std::string message;
    std::size_t offset{};
};

struct ManifestParseResult {
    std::optional<ModManifest> manifest;
    std::optional<ManifestError> error;

    [[nodiscard]] bool ok() const noexcept { return manifest.has_value(); }
};

[[nodiscard]] ManifestParseResult parse_manifest(std::string_view json);

} // namespace enrage::core
