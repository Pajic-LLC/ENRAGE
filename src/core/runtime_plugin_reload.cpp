#include "core/runtime.hpp"

#include <algorithm>

namespace enrage::core {

bool Runtime::reload_plugin(
    std::string_view plugin_id,
    const std::optional<std::filesystem::path>& replacement_path) {
    if (!running_ || recovery_.safe_mode() || !extensions_active_ ||
        adapter_->environment_state() != adapters::EnvironmentState::local) {
        return false;
    }

    const auto found = std::ranges::find(plugins_.plugins(), plugin_id, &LoadedPlugin::id);
    if (found == plugins_.plugins().end()) return false;

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

} // namespace enrage::core
