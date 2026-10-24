#include "adapters/null_adapter.hpp"
#include "core/mod_registry.hpp"
#include "core/overlay.hpp"
#include "core/runtime.hpp"
#include "platform/signature/pattern.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>

namespace {

void pattern_scanner_supports_wildcards() {
    const auto pattern = enrage::platform::signature::Pattern::parse("48 8B ?? 89");
    assert(pattern.has_value());

    const std::array bytes{
        std::byte{0x90}, std::byte{0x48}, std::byte{0x8B},
        std::byte{0x12}, std::byte{0x89}, std::byte{0x90},
    };

    const auto match = pattern->find(bytes);
    assert(match.has_value());
    assert(*match == 1);
}

void registry_discovers_directories_and_packages() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-registry-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "alpha");
    std::ofstream{root / "beta.enr"} << "test";
    std::ofstream{root / "ignored.txt"} << "test";

    enrage::core::ModRegistry registry{root};
    registry.discover();

    assert(registry.candidates().size() == 2);
    assert(registry.candidates()[0].id == "alpha");
    assert(registry.candidates()[1].id == "beta");
    assert(registry.candidates()[1].packaged);

    std::filesystem::remove_all(root);
}

void overlay_prefers_highest_priority_and_rejects_traversal() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-overlay-test";
    const auto low = root / "low";
    const auto high = root / "high";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(low / "data");
    std::filesystem::create_directories(high / "data");
    std::ofstream{low / "data" / "value.bin"} << "low";
    std::ofstream{high / "data" / "value.bin"} << "high";

    enrage::core::Overlay overlay;
    assert(overlay.mount({"low", low, 10}));
    assert(overlay.mount({"high", high, 20}));
    assert(!overlay.mount({"high", high, 30}));

    const auto resolved = overlay.resolve("data/value.bin");
    assert(resolved.has_value());
    assert(*resolved == high / "data" / "value.bin");
    assert(!overlay.resolve("../outside.bin").has_value());

    assert(overlay.unmount("high"));
    const auto fallback = overlay.resolve("data/value.bin");
    assert(fallback.has_value());
    assert(*fallback == low / "data" / "value.bin");

    std::filesystem::remove_all(root);
}

void runtime_lifecycle_is_idempotent() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-runtime-test";
    std::filesystem::remove_all(root);

    enrage::core::Runtime runtime{
        std::make_unique<enrage::adapters::NullAdapter>(), root};

    assert(runtime.start());
    assert(runtime.running());
    assert(!runtime.start());

    runtime.stop();
    assert(!runtime.running());
    runtime.stop();

    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    pattern_scanner_supports_wildcards();
    registry_discovers_directories_and_packages();
    overlay_prefers_highest_priority_and_rejects_traversal();
    runtime_lifecycle_is_idempotent();
    return 0;
}
