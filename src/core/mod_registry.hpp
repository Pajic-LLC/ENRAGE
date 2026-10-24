#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace enrage::core {

struct ModCandidate {
    std::filesystem::path path;
    std::string id;
    bool packaged{};
};

class ModRegistry {
public:
    explicit ModRegistry(std::filesystem::path root);

    void discover();

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] const std::vector<ModCandidate>& candidates() const noexcept;

private:
    std::filesystem::path root_;
    std::vector<ModCandidate> candidates_;
};

} // namespace enrage::core
