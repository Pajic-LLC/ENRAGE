#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enrage::platform {
class ExecutionJournal;
}

namespace enrage::core {

struct RecoveryBreadcrumb {
    std::string mod_id;
    std::string plugin_id;
    std::string callback;
    std::uint64_t execution_id{};
    std::uint64_t thread_id{};
    std::uint64_t generation{};
    std::uint64_t worker_id{};
};

class RecoveryManager {
public:
    explicit RecoveryManager(std::filesystem::path state_root);
    ~RecoveryManager();

    RecoveryManager(const RecoveryManager&) = delete;
    RecoveryManager& operator=(const RecoveryManager&) = delete;

    bool begin_session(bool force_safe_mode = false);
    void end_session() noexcept;

    bool enter_mod(std::string_view mod_id);
    bool leave_mod();
    bool enter_execution(
        std::string_view mod_id,
        std::string_view plugin_id,
        std::string_view callback,
        std::uint64_t generation = 0,
        std::uint64_t worker_id = 0);
    bool leave_execution();

    [[nodiscard]] bool safe_mode() const noexcept;
    [[nodiscard]] bool previous_session_incomplete() const noexcept;
    [[nodiscard]] const std::optional<std::string>& suspected_mod() const noexcept;
    [[nodiscard]] const std::optional<RecoveryBreadcrumb>& suspected_execution() const noexcept;
    [[nodiscard]] const std::vector<RecoveryBreadcrumb>& active_executions_at_crash() const noexcept;
    [[nodiscard]] bool quarantined(std::string_view mod_id) const noexcept;
    [[nodiscard]] const std::vector<std::string>& quarantined_mods() const noexcept;

    bool clear_quarantine(std::string_view mod_id);
    bool clear_all_quarantine();

private:
    bool load_quarantine();
    bool save_quarantine() const;
    bool write_session_marker() const;
    bool write_execution_ledger_locked();
    bool recover_previous_session();
    bool begin_crash_witness();
    void end_crash_witness() noexcept;

    std::filesystem::path state_root_;
    std::filesystem::path session_path_;
    std::filesystem::path execution_path_;
    std::filesystem::path crash_path_;
    std::filesystem::path quarantine_path_;
    std::vector<std::string> quarantined_;
    std::vector<RecoveryBreadcrumb> active_executions_;
    std::vector<RecoveryBreadcrumb> active_executions_at_crash_;
    std::optional<std::string> suspected_mod_;
    std::optional<RecoveryBreadcrumb> suspected_execution_;
    std::unique_ptr<platform::ExecutionJournal> execution_journal_;
    mutable std::mutex execution_mutex_;
    bool safe_mode_{};
    bool previous_session_incomplete_{};
    bool session_active_{};
    bool crash_witness_active_{};
};

} // namespace enrage::core
