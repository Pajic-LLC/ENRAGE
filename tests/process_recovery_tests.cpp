#include "adapters/null_adapter.hpp"
#include "core/recovery.hpp"
#include "core/runtime.hpp"

#include <cassert>
#include <filesystem>
#include <memory>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

int run_child(
    const std::filesystem::path& probe,
    const std::filesystem::path& root,
    const std::filesystem::path& plugin) {
#ifdef _WIN32
    std::wstring command = L"\"" + probe.wstring() + L"\" \"" + root.wstring() +
                           L"\" \"" + plugin.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    if (!CreateProcessW(
            probe.c_str(),
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        return -1;
    }

    const DWORD wait = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0;
    const BOOL have_code = GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || !have_code) return -1;
    return static_cast<int>(exit_code);
#else
    const pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        execl(
            probe.c_str(),
            probe.c_str(),
            root.c_str(),
            plugin.c_str(),
            static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    if (waitpid(child, &status, 0) != child) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
#endif
}

} // namespace

int main(int argc, char** argv) {
    assert(argc == 3);
    const std::filesystem::path probe = argv[1];
    const std::filesystem::path plugin = argv[2];
    const auto root = std::filesystem::temp_directory_path() / "enrage-process-recovery-test";
    std::filesystem::remove_all(root);

    const int result = run_child(probe, root, plugin);
    assert(result >= 0);
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
