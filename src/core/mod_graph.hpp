#pragma once

#include <span>
#include <string>
#include <vector>

namespace enrage::core {

enum class ModIssueKind {
    duplicate_id,
    missing_dependency,
    conflict,
    cycle,
};

struct ModIssue {
    ModIssueKind kind{};
    std::string source;
    std::string target;
};

struct ModSpec {
    std::string id;
    int priority{};
    std::vector<std::string> dependencies;
    std::vector<std::string> load_after;
    std::vector<std::string> conflicts;
};

struct ModResolution {
    std::vector<std::string> order;
    std::vector<ModIssue> issues;

    [[nodiscard]] bool ok() const noexcept;
};

class ModGraph {
public:
    [[nodiscard]] static ModResolution resolve(std::span<const ModSpec> mods);
};

} // namespace enrage::core
