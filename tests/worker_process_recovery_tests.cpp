#include "core/recovery.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

struct ChildResult {
    bool valid{};
    std::uint32_t exit_code{};
};

ChildResult run_child(
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
        return {};
    }

    const DWORD wait = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0;
    const BOOL have_code = GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || !have_code) return {};
    return {.valid = true, .exit_code = static_cast<std::uint32_t>(exit_code)};
#else
    const pid_t child = fork();
    if (child < 0) return {};
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
    if (waitpid(child, &status, 0) != child) return {};
    if (WIFEXITED(status)) {
        return {.valid = true, .exit_code = static_cast<std::uint32_t>(WEXITSTATUS(status))};
    }
    if (WIFSIGNALED(status)) {
        return {
            .valid = true,
            .exit_code = static_cast<std::uint32_t>(128 + WTERMSIG(status)),
        };
    }
    return {};
#endif
}

} // namespace

int main(int argc, char** argv) {
    assert(argc == 3);
    const std::filesystem::path probe = argv[1];
    const std::filesystem::path plugin = argv[2];
    const auto root = std::filesystem::temp_directory_path() / "enrage-worker-process-recovery-test";
    std::filesystem::remove_all(root);

    const auto result = run_child(probe, root, plugin);
    assert(result.valid);
    assert(result.exit_code != 0);

    enrage::core::RecoveryManager recovery{root / ".enrage"};
    assert(recovery.begin_session());
    assert(recovery.previous_session_incomplete());
    assert(!recovery.safe_mode());
    assert(recovery.suspected_mod().has_value());
    assert(*recovery.suspected_mod() == "example.crash");
    assert(recovery.suspected_execution().has_value());

    const auto& suspected = *recovery.suspected_execution();
    assert(suspected.mod_id == "example.crash");
    assert(suspected.plugin_id == "dev.enrage.worker-fault");
    assert(suspected.callback == "worker");
    assert(suspected.execution_id != 0);
    assert(suspected.thread_id != 0);
    assert(suspected.generation != 0);
    assert(suspected.worker_id != 0);

    const auto& active = recovery.active_executions_at_crash();
    assert(active.size() >= 2);
    const auto tick = std::ranges::find_if(active, [](const auto& entry) {
        return entry.plugin_id == "dev.enrage.worker-fault" && entry.callback == "tick";
    });
    assert(tick != active.end());
    assert(tick->execution_id != suspected.execution_id);
    assert(tick->thread_id != suspected.thread_id);
    assert(tick->generation == suspected.generation);
    assert(tick->worker_id == 0);
    assert(recovery.quarantined("example.crash"));

    recovery.end_session();
    std::filesystem::remove_all(root);
    return 0;
}
