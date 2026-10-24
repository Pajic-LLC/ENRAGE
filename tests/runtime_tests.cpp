#include "adapters/null_adapter.hpp"
#include "core/mod_registry.hpp"
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
    runtime_lifecycle_is_idempotent();
    return 0;
}
