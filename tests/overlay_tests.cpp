#include "core/archive.hpp"
#include "core/overlay.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace {

const enrage::core::OverlayEntry* find_entry(
    const std::vector<enrage::core::OverlayEntry>& entries,
    std::string_view name) {
    for (const auto& entry : entries) {
        if (entry.name.generic_string() == name) return &entry;
    }
    return nullptr;
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-overlay-conflict-test";
    const auto low = root / "low";
    const auto high_source = root / "high-source";
    const auto high_package = root / "high.enr";
    std::filesystem::remove_all(root);

    std::filesystem::create_directories(low / "data" / "nested");
    std::ofstream{low / "data" / "same.bin"} << "low";
    std::ofstream{low / "data" / "only-low.bin"} << "low";
    std::ofstream{low / "data" / "nested" / "low.txt"} << "low";
    std::filesystem::create_directories(low / "data" / "clash");
    std::ofstream{low / "data" / "clash" / "child.txt"} << "lower-dir";
    std::ofstream{low / "data" / "dir-shadow"} << "lower-file";

    std::filesystem::create_directories(high_source / "files" / "data" / "nested");
    std::filesystem::create_directories(high_source / "files" / "data" / "dir-shadow");
    std::ofstream{high_source / "mod.json"} << R"({"id":"example.overlay.high"})";
    std::ofstream{high_source / "files" / "data" / "same.bin"} << "high";
    std::ofstream{high_source / "files" / "data" / "only-high.bin"} << "high";
    std::ofstream{high_source / "files" / "data" / "nested" / "high.txt"} << "high";
    std::ofstream{high_source / "files" / "data" / "clash"} << "higher-file";
    std::ofstream{high_source / "files" / "data" / "dir-shadow" / "child.txt"} << "higher-dir";
    assert(enrage::core::Archive::create(high_source, high_package));

    enrage::core::Overlay overlay;
    assert(overlay.mount({"low", low, 1}));
    assert(overlay.mount_archive("high", high_package, "files", 2));

    const auto conflicts = overlay.conflicts();
    assert(conflicts.size() == 1);
    assert(conflicts[0].logical_path.generic_string() == "data/same.bin");
    assert(conflicts[0].mounts.size() == 2);
    assert(conflicts[0].mounts[0] == "high");
    assert(conflicts[0].mounts[1] == "low");

    assert(overlay.read_text("data/same.bin") == "high");
    assert(!overlay.resolve("data/same.bin").has_value());
    assert(!overlay.resolve("../../outside.bin").has_value());
    assert(!overlay.read("../../outside.bin").has_value());

    const auto root_entries = overlay.list();
    assert(root_entries.size() == 1);
    assert(root_entries[0].name == "data");
    assert(root_entries[0].kind == enrage::core::OverlayEntryKind::directory);
    assert(root_entries[0].mount_id == "high");

    const auto data_entries = overlay.list("data");
    const auto* same = find_entry(data_entries, "same.bin");
    const auto* only_high = find_entry(data_entries, "only-high.bin");
    const auto* only_low = find_entry(data_entries, "only-low.bin");
    const auto* nested = find_entry(data_entries, "nested");
    const auto* clash = find_entry(data_entries, "clash");
    const auto* dir_shadow = find_entry(data_entries, "dir-shadow");
    assert(same && same->kind == enrage::core::OverlayEntryKind::file && same->mount_id == "high");
    assert(only_high && only_high->mount_id == "high");
    assert(only_low && only_low->mount_id == "low");
    assert(nested && nested->kind == enrage::core::OverlayEntryKind::directory && nested->mount_id == "high");
    assert(clash && clash->kind == enrage::core::OverlayEntryKind::file && clash->mount_id == "high");
    assert(dir_shadow && dir_shadow->kind == enrage::core::OverlayEntryKind::directory && dir_shadow->mount_id == "high");

    const auto nested_entries = overlay.list("data/nested");
    assert(find_entry(nested_entries, "high.txt"));
    assert(find_entry(nested_entries, "low.txt"));

    // High-priority file shadows lower directory, and high-priority directory
    // shadows lower file. All lookup operations agree on those types.
    assert(overlay.list("data/clash").empty());
    assert(!overlay.read("data/dir-shadow").has_value());
    assert(!overlay.resolve("data/dir-shadow").has_value());
    const auto shadowed_dir = overlay.list("data/dir-shadow");
    assert(shadowed_dir.size() == 1);
    assert(shadowed_dir[0].name == "child.txt");
    assert(shadowed_dir[0].mount_id == "high");
    assert(overlay.list("../outside").empty());

    assert(overlay.unmount("high"));
    assert(overlay.read_text("data/same.bin") == "low");
    const auto low_clash = overlay.list("data/clash");
    assert(low_clash.size() == 1);
    assert(low_clash[0].name == "child.txt");
    assert(low_clash[0].mount_id == "low");
    assert(overlay.read_text("data/dir-shadow") == "lower-file");

    std::filesystem::remove_all(root);
    return 0;
}
