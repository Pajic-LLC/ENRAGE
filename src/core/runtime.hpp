#pragma once

#include "adapters/adapter.hpp"
#include "core/mod_registry.hpp"
#include "core/overlay.hpp"
#include "core/package.hpp"
#include "core/plugin_host.hpp"
#include "core/recovery.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enrage::core {

struct RuntimeSnapshot {
    bool running{};
    bool extensions_active{};
    bool safe_mode{};
    std::string adapter_name;
    adapters::EnvironmentState environment{adapters::EnvironmentState::unknown};
    adapters::BuildIdentity build;
    std::uint64_t capabilities{};
    adapters::AdapterHealthSnapshot health;
    std::vector<ModCandidate> mod_candidates;
    std::vector<LoadedMod> loaded_mods;
    std::vector<PluginSnapshot> plugins;
    std::vector<std::string> quarantined_mods;
    std::vector<Diagnostic> diagnostics;
    std::optional<RecoveryBreadcrumb> suspected_execution;
};

class Runtime {
public:
    Runtime(std::unique_ptr<adapters::Adapter> adapter, std::filesystem::path mod_root);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool start(bool force_safe_mode = false);
    void stop() noexcept;
    void tick(double delta_seconds);
    bool reload_support();
    bool reload_mods();
    bool reload_plugin(
        std::string_view plugin_id,
        const std::optional<std::filesystem::path>& replacement_path = std::nullopt) {
        std::scoped_lock lock(runtime_mutex_);
        if (!running_ || recovery_.safe_mode() || !extensions_active_ ||
            adapter_->environment_state() != adapters::EnvironmentState::local) {
            return false;
        }

        const auto live_plugins = plugins_.snapshot();
        const auto found = std::ranges::find(live_plugins, plugin_id, &PluginSnapshot::id);
        if (found == live_plugins.end()) return false;

        const auto source = replacement_path ? *replacement_path : found->path;
        const auto owner_mod_id = found->owner_mod_id;
        if (plugins_.reload(plugin_id, replacement_path)) return true;

        diagnostics_.push_back({
            DiagnosticSeverity::error,
            DiagnosticCode::plugin_reload_failed,
            source,
            owner_mod_id,
            "failed to hot reload native plugin: " + std::string{plugin_id},
        });
        return false;
    }

    [[nodiscard]] RuntimeSnapshot snapshot() const {
        std::scoped_lock lock(runtime_mutex_);
        RuntimeSnapshot result;
        result.running = running_;
        result.extensions_active = extensions_active_;
        result.safe_mode = recovery_.safe_mode();
        result.capabilities = capabilities_;
        result.health = health_;
        result.mod_candidates = mods_.candidates();
        result.loaded_mods = loaded_mods_;
        result.plugins = plugins_.snapshot();
        result.quarantined_mods = recovery_.quarantined_mods();
        result.diagnostics = diagnostics_;
        result.suspected_execution = recovery_.suspected_execution();
        if (adapter_) {
            result.adapter_name = std::string{adapter_->name()};
            result.environment = adapter_->environment_state();
            result.build = adapter_->build();
        }
        return result;
    }

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool extensions_active() const noexcept;
    [[nodiscard]] bool safe_mode() const noexcept;
    [[nodiscard]] std::uint64_t capabilities() const noexcept;
    [[nodiscard]] const adapters::AdapterHealthSnapshot& health() const noexcept;
    [[nodiscard]] const ModRegistry& mods() const noexcept;
    [[nodiscard]] const std::vector<LoadedMod>& loaded_mods() const noexcept;
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept;
    [[nodiscard]] const Overlay& overlay() const noexcept;
    [[nodiscard]] const PluginHost& plugins() const noexcept;
    [[nodiscard]] const std::vector<std::string>& quarantined_mods() const noexcept;
    [[nodiscard]] const std::optional<RecoveryBreadcrumb>& suspected_execution() const noexcept;
    [[nodiscard]] const adapters::Adapter& adapter() const noexcept;

    bool clear_quarantine(std::string_view mod_id);
    bool clear_all_quarantine();

private:
    void activate_extensions();
    void deactivate_extensions() noexcept;
    void mount_content();
    void sync_capabilities();
    void sync_health(bool initial = false);
    void report_recovery_state_error(std::string message);
    [[nodiscard]] bool activation_failed_since(std::size_t diagnostic_index) const noexcept;

    std::unique_ptr<adapters::Adapter> adapter_;
    ModRegistry mods_;
    std::filesystem::path state_root_;
    RecoveryManager recovery_;
    std::vector<LoadedMod> loaded_mods_;
    std::vector<Diagnostic> diagnostics_;
    std::vector<std::string> load_order_;
    Overlay overlay_;
    PluginHost plugins_;
    adapters::AdapterHealthSnapshot health_;
    std::filesystem::path active_reload_state_;
    std::uint64_t reload_generation_{};
    std::uint64_t capabilities_{};
    bool package_set_valid_{};
    bool conflicts_reported_{};
    bool extensions_active_{};
    bool recovery_state_error_reported_{};
    bool running_{};
    mutable std::mutex runtime_mutex_;
};

} // namespace enrage::core
