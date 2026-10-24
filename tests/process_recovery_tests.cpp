#include "adapters/null_adapter.hpp"
#include "core/recovery.hpp"
#include "core/runtime.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace {

std::string quote(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

} // namespace

int main(int argc, char** argv) {
    assert(argc == 3);
    const std::filesystem::path probe = argv[1];
    const std::filesystem::path plugin = argv[2];
    const auto root = std::filesystem::temp_directory_path() / "enrage-process-recovery-test";
    std::filesystem::remove_all(root);

    const auto command = quote(probe) + " " + quote(root) + " " + quote(plugin);
    const int result = std::system(command.c_str());
    assert(result != 0);

    {
        enrage::core::RecoveryManager recovery{root / ".enrage"};
        assert(recovery.begin_session());
        assert(recovery.previous_session_incomplete());
        assert(!recovery.safe_mode());
        assert(recovery.suspected_mod().has_value());
        assert(*recovery.suspected_mod() == "example.crash");
        assert(recovery.suspected_execution().has_value());
        assert(recovery.suspected_execution()->mod_id == "example.crash");
        assert(recovery.suspected_execution()->plugin_id == "dev.enrage.fault");
        assert(recovery.suspected_execution()->callback == "tick");
        assert(recovery.quarantined("example.crash"));
        recovery.end_session();
    }

    {
        enrage::core::Runtime runtime{
            std::make_unique<enrage::adapters::NullAdapter>(), root / "mods"};
        assert(runtime.start());
        assert(runtime.quarantined_mods().size() == 1);
        assert(runtime.quarantined_mods()[0] == "example.crash");
        assert(runtime.plugins().plugins().empty());
        assert(runtime.extensions_active());
        runtime.tick(1.0 / 60.0);
        runtime.stop();
    }

    std::filesystem::remove_all(root);
    return 0;
}
