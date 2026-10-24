#include "adapters/adapter.hpp"
#include "core/runtime.hpp"
#include "enrage/sdk.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>

namespace {

class MutableAdapter final : public enrage::adapters::Adapter {
public:
    std::string_view name() const noexcept override { return "test"; }
    bool probe() override { return true; }
    enrage::adapters::BuildIdentity build() const override { return {"test", 1}; }
    std::uint64_t capabilities() const noexcept override { return ENRAGE_CAPABILITY_VIRTUAL_FILES; }
    enrage::adapters::EnvironmentState environment_state() const noexcept override { return state; }
    bool initialize() override { return true; }
    void shutdown() noexcept override {}

    enrage::adapters::EnvironmentState state{enrage::adapters::EnvironmentState::local};
};

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-environment-test";
    const auto mods = root / "mods";
    const auto mod = mods / "example";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(mod / "files" / "data");
    std::ofstream{mod / "mod.json"} << R"({"id":"example.environment"})";
    std::ofstream{mod / "files" / "data" / "value.txt"} << "active";

    auto adapter = std::make_unique<MutableAdapter>();
    auto* mutable_adapter = adapter.get();
    enrage::core::Runtime runtime{std::move(adapter), mods};

    assert(runtime.start());
    assert(runtime.extensions_active());
    assert(runtime.overlay().resolve("data/value.txt").has_value());

    mutable_adapter->state = enrage::adapters::EnvironmentState::restricted;
    runtime.tick(0.0);
    assert(!runtime.extensions_active());
    assert(!runtime.overlay().resolve("data/value.txt").has_value());

    mutable_adapter->state = enrage::adapters::EnvironmentState::unknown;
    runtime.tick(0.0);
    assert(!runtime.extensions_active());

    mutable_adapter->state = enrage::adapters::EnvironmentState::local;
    runtime.tick(0.0);
    assert(runtime.extensions_active());
    assert(runtime.overlay().resolve("data/value.txt").has_value());

    runtime.stop();
    assert(!runtime.extensions_active());
    std::filesystem::remove_all(root);
    return 0;
}
