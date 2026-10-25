#include "adapters/null_adapter.hpp"
#include "core/recovery.hpp"
#include "core/runtime.hpp"
#include "platform/atomic_file.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input);
    return std::string{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
}

void atomic_state_replacement_preserves_previous_file_on_failure() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-atomic-state-test";
    const auto state = root / "state.dat";
    std::filesystem::remove_all(root);

    assert(enrage::platform::atomic_replace_file(state, "first"));
    assert(read_text(state) == "first");
    assert(enrage::platform::atomic_replace_file(state, "second"));
    assert(read_text(state) == "second");

    auto temporary = state;
    temporary += ".tmp";
    assert(std::filesystem::create_directory(temporary));
    assert(!enrage::platform::atomic_replace_file(state, "must-not-replace"));
    assert(read_text(state) == "second");
    assert(std::filesystem::is_directory(temporary));

    std::filesystem::remove_all(temporary);
    const auto nested = root / "nested" / "state.dat";
    assert(enrage::platform::atomic_replace_file(nested, "nested"));
    assert(read_text(nested) == "nested");
    auto nested_temporary = nested;
    nested_temporary += ".tmp";
    assert(!std::filesystem::exists(nested_temporary));

    std::filesystem::remove_all(root);
}

void quarantines_mod_active_during_incomplete_session() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-recovery-quarantine-test";
    std::filesystem::remove_all(root);

    {
        enrage::core::RecoveryManager first{root};
        assert(first.begin_session());
        assert(first.enter_mod("example.bad"));
    }

    {
        enrage::core::RecoveryManager second{root};
        assert(second.begin_session());
        assert(second.previous_session_incomplete());
        assert(!second.safe_mode());
        assert(second.suspected_mod().has_value());
        assert(*second.suspected_mod() == "example.bad");
        assert(second.quarantined("example.bad"));
        second.end_session();
    }

    {
        enrage::core::RecoveryManager third{root};
        assert(third.begin_session());
        assert(!third.previous_session_incomplete());
        assert(third.quarantined("example.bad"));
        assert(third.clear_quarantine("example.bad"));
        assert(!third.quarantined("example.bad"));
        third.end_session();
    }

    std::filesystem::remove_all(root);
}

void unattributed_incomplete_session_enters_safe_mode() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-recovery-safe-test";
    std::filesystem::remove_all(root);

    {
        enrage::core::RecoveryManager first{root};
        assert(first.begin_session());
    }

    {
        enrage::core::RecoveryManager second{root};
        assert(second.begin_session());
        assert(second.previous_session_incomplete());
        assert(second.safe_mode());
        assert(!second.suspected_mod().has_value());
        second.end_session();
    }

    std::filesystem::remove_all(root);
}

void explicit_safe_mode_is_one_session_policy() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-recovery-explicit-safe-test";
    std::filesystem::remove_all(root);

    {
        enrage::core::RecoveryManager first{root};
        assert(first.begin_session(true));
        assert(first.safe_mode());
        first.end_session();
    }

    {
        enrage::core::RecoveryManager second{root};
        assert(second.begin_session());
        assert(!second.safe_mode());
        second.end_session();
    }

    std::filesystem::remove_all(root);
}

void runtime_safe_mode_keeps_extensions_unmounted() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-runtime-safe-mode-test";
    const auto mods = root / "mods";
    const auto mod = mods / "example";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(mod / "files" / "data");
    std::ofstream{mod / "mod.json"} << R"({"id":"example.safe"})";
    std::ofstream{mod / "files" / "data" / "value.txt"} << "mounted";

    {
        enrage::core::Runtime runtime{
            std::make_unique<enrage::adapters::NullAdapter>(), mods};
        assert(runtime.start(true));
        assert(runtime.safe_mode());
        assert(!runtime.extensions_active());
        assert(!runtime.overlay().resolve("data/value.txt").has_value());
        runtime.stop();
    }

    {
        enrage::core::Runtime runtime{
            std::make_unique<enrage::adapters::NullAdapter>(), mods};
        assert(runtime.start());
        assert(!runtime.safe_mode());
        assert(runtime.extensions_active());
        assert(runtime.overlay().resolve("data/value.txt").has_value());
        runtime.stop();
    }

    std::filesystem::remove_all(root);
}

void runtime_skips_persistently_quarantined_mod() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-runtime-quarantine-test";
    const auto mods = root / "mods";
    const auto mod = mods / "bad";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(mod / "files" / "data");
    std::ofstream{mod / "mod.json"} << R"({"id":"example.bad"})";
    std::ofstream{mod / "files" / "data" / "value.txt"} << "should-not-mount";

    {
        enrage::core::RecoveryManager interrupted{root / ".enrage"};
        assert(interrupted.begin_session());
        assert(interrupted.enter_mod("example.bad"));
    }

    {
        enrage::core::Runtime runtime{
            std::make_unique<enrage::adapters::NullAdapter>(), mods};
        assert(runtime.start());
        assert(!runtime.safe_mode());
        assert(runtime.quarantined_mods().size() == 1);
        assert(runtime.quarantined_mods()[0] == "example.bad");
        assert(!runtime.overlay().resolve("data/value.txt").has_value());
        runtime.stop();
    }

    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    atomic_state_replacement_preserves_previous_file_on_failure();
    quarantines_mod_active_during_incomplete_session();
    unattributed_incomplete_session_enters_safe_mode();
    explicit_safe_mode_is_one_session_policy();
    runtime_safe_mode_keeps_extensions_unmounted();
    runtime_skips_persistently_quarantined_mod();
    return 0;
}
