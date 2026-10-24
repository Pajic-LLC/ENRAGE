#include "core/overlay.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>

int main() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-overlay-conflict-test";
    const auto low = root / "low";
    const auto high = root / "high";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(low / "data");
    std::filesystem::create_directories(high / "data");
    std::ofstream{low / "data" / "same.bin"} << "low";
    std::ofstream{high / "data" / "same.bin"} << "high";
    std::ofstream{high / "data" / "only-high.bin"} << "high";

    enrage::core::Overlay overlay;
    assert(overlay.mount({"low", low, 1}));
    assert(overlay.mount({"high", high, 2}));

    const auto conflicts = overlay.conflicts();
    assert(conflicts.size() == 1);
    assert(conflicts[0].logical_path.generic_string() == "data/same.bin");
    assert(conflicts[0].mounts.size() == 2);
    assert(conflicts[0].mounts[0] == "high");
    assert(conflicts[0].mounts[1] == "low");

    const auto resolved = overlay.resolve("data/same.bin");
    assert(resolved.has_value());
    assert(resolved->filename() == "same.bin");
    assert(!overlay.resolve("../../outside.bin").has_value());

    std::filesystem::remove_all(root);
    return 0;
}
