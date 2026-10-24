#include "core/mod_registry.hpp"

#include <algorithm>
#include <system_error>

namespace enrage::core {

ModRegistry::ModRegistry(std::filesystem::path root) : root_(std::move(root)) {}

void ModRegistry::discover() {
    candidates_.clear();

    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (error) {
        return;
    }

    for (std::filesystem::directory_iterator it(root_, error), end; !error && it != end; it.increment(error)) {
        const auto& entry = *it;
        const auto path = entry.path();

        if (entry.is_directory(error)) {
            candidates_.push_back({path, path.filename().string(), false});
            continue;
        }

        if (entry.is_regular_file(error) && path.extension() == ".enr") {
            candidates_.push_back({path, path.stem().string(), true});
        }
    }

    std::ranges::sort(candidates_, {}, [](const ModCandidate& candidate) {
        return candidate.id;
    });
}

const std::filesystem::path& ModRegistry::root() const noexcept {
    return root_;
}

const std::vector<ModCandidate>& ModRegistry::candidates() const noexcept {
    return candidates_;
}

} // namespace enrage::core
