#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enrage::core {

class RecoveryManager {
public:
    explicit RecoveryManager(std::filesystem::path state_root);

    bool begin_session(bool force_safe_mode = false);
    void end_session() noexcept;

    bool enter_mod(std::string_view mod_id);
    bool leave_mod();

    [[nodiscard]] bool safe_mode() const noexcept;
    [[nodiscard]] bool previous_session_incomplete() const noexcept;
    [[nodiscard]] const std::optional<std::string>& suspected_mod() const noexcept;
    [[nodiscard]] bool quarantined(std::string_view mod_id) const noexcept;
    [[nodiscard]] const std::vector<std::string>& quarantined_mods() const noexcept;

    bool clear_quarantine(std::string_view mod_id);
    bool clear_all_quarantine();

private:
    bool load_quarantine();
    bool save_quarantine() const;
    bool write_session(std::string_view active_mod) const;
    bool recover_previous_session();

    std::filesystem::path state_root_;
    std::filesystem::path session_path_;
    std::filesystem::path quarantine_path_;
    std::vector<std::string> quarantined_;
    std::optional<std::string> suspected_mod_;
    bool safe_mode_{};
    bool previous_session_incomplete_{};
    bool session_active_{};
};

} // namespace enrage::core
