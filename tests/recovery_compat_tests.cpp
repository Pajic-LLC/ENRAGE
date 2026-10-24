#include "core/recovery.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>

int main() {
    const auto root = std::filesystem::temp_directory_path() / "enrage-recovery-compat-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    // Legacy v1 encoded "example.legacy" in hexadecimal.
    std::ofstream{root / "session.state"}
        << "version=1\n"
        << "active=6578616d706c652e6c6567616379\n";

    enrage::core::RecoveryManager recovery{root};
    assert(recovery.begin_session());
    assert(recovery.previous_session_incomplete());
    assert(!recovery.safe_mode());
    assert(recovery.suspected_mod().has_value());
    assert(*recovery.suspected_mod() == "example.legacy");
    assert(recovery.suspected_execution().has_value());
    assert(recovery.suspected_execution()->mod_id == "example.legacy");
    assert(recovery.suspected_execution()->plugin_id.empty());
    assert(recovery.suspected_execution()->callback == "activation");
    assert(recovery.quarantined("example.legacy"));
    recovery.end_session();

    std::filesystem::remove_all(root);
    return 0;
}
