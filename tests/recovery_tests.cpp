#include "core/recovery.hpp"

#include <cassert>
#include <filesystem>

namespace {

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

} // namespace

int main() {
    quarantines_mod_active_during_incomplete_session();
    unattributed_incomplete_session_enters_safe_mode();
    explicit_safe_mode_is_one_session_policy();
    return 0;
}
