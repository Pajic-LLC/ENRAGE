#include "core/runtime.hpp"

#include "core/mod_graph.hpp"
#include "enrage/sdk.h"

#include <algorithm>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace enrage::core {
namespace {

std::string issue_message(const ModIssue& issue) {
    switch (issue.kind) {
        case ModIssueKind::duplicate_id:
            return "duplicate mod id: " + issue.source;
        case ModIssueKind::missing_dependency:
            return issue.source + " requires missing dependency " + issue.target;
        case ModIssueKind::conflict:
            return issue.source + " conflicts with " + issue.target;
        case ModIssueKind::cycle:
            return "dependency cycle includes " + issue.source;
    }
    return "invalid mod graph";
}

std::string conflict_message(const OverlayConflict& conflict) {
    std::string message = "multiple mods provide " + conflict.logical_path.generic_string() + ": ";
    for (std::size_t i = 0; i < conflict.mounts.size(); ++i) {
        if (i != 0) message += ", ";
        message += conflict.mounts[i];
    }
    if (!conflict.mounts.empty()) message += " (first has priority)";
    return message;
}

std::string recovery_message(const RecoveryBreadcrumb& breadcrumb) {
    std::string message = "previous session ended while this mod was executing";
    if (!breadcrumb.plugin_id.empty()) message += " plugin " + breadcrumb.plugin_id;
    if (!breadcrumb.callback.empty()) message += " callback " + breadcrumb.callback;
    message += "; it has been quarantined";
    return message;
}

std::string anchor_state_name(adapters::AnchorHealthState state) {
    switch (state) {
        case adapters::AnchorHealthState::unresolved: return "unresolved";
        case adapters::AnchorHealthState::resolved: return "resolved";
        case adapters::AnchorHealthState::degraded: return "degraded";
        case adapters::AnchorHealthState::missing: return "missing";
    }
    return "unresolved";
}

DiagnosticSeverity anchor_severity(adapters::AnchorHealthState state) {
    switch (state) {
        case adapters::AnchorHealthState::resolved: return DiagnosticSeverity::info;
        case adapters::AnchorHealthState::degraded: return DiagnosticSeverity::warning;
        case adapters::AnchorHealthState::missing: return DiagnosticSeverity::warning;
        case adapters::AnchorHealthState::unresolved: return DiagnosticSeverity::warning;
    }
    return DiagnosticSeverity::warning;
}

std::string capability_name(std::uint64_t capability) {
    switch (capability) {
        case ENRAGE_CAPABILITY_TICK: return "tick";
        case ENRAGE_CAPABILITY_VIRTUAL_FILES: return "virtual-files";
        case ENRAGE_CAPABILITY_NATIVE_CALLS: return "native-calls";
        case ENRAGE_CAPABILITY_ASSETS: return "assets";
        default: return "capability-" + std::to_string(capability);
    }
}

std::string join_names(const std::vector<std::string>& names) {
    std::string result;
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (index != 0) result += ", ";
        result += names[index];
    }
    return result;
}

const adapters::AnchorHealth* find_anchor(
    const adapters::AdapterHealthSnapshot& snapshot,
    std::string_view name) {
    const auto found = std::ranges::find(snapshot.anchors, name, &adapters::AnchorHealth::name);
    return found == snapshot.anchors.end() ? nullptr : &*found;
}

const adapters::CapabilityHealth* find_capability(
    const adapters::AdapterHealthSnapshot& snapshot,
    std::uint64_t capability) {
    const auto found = std::ranges::find(
        snapshot.capabilities,
        capability,
        &adapters::CapabilityHealth::capability);
    return found == snapshot.capabilities.end() ? nullptr : &*found;
}

std::string anchor_message(const adapters::AnchorHealth& anchor) {
    std::string message = "anchor " + anchor.name + " is " + anchor_state_name(anchor.state);
    if (!anchor.reason.empty()) message += ": " + anchor.reason;
    return message;
}

std::string capability_message(const adapters::CapabilityHealth& capability) {
    std::string message = "capability " + capability_name(capability.capability);
    if (!capability.advertised) {
        message += " is not advertised by the adapter";
        return message;
    }
    if (!capability.available) {
        message += " is suppressed";
        if (!capability.blocking_anchors.empty()) message += " by " + join_names(capability.blocking_anchors);
        return message;
    }
    if (!capability.degraded_anchors.empty()) {
        message += " is available with degraded anchors: " + join_names(capability.degraded_anchors);
        return message;
    }
    message += " is available";
    return message;
}

bool contains_error(const std::vector<Diagnostic>& diagnostics) {
    return std::ranges::any_of(diagnostics, [](const Diagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::error;
    });
}

} // namespace

Runtime::Runtime(std::unique_ptr<adapters::Adapter> adapter, std::filesystem::path mod_root)
    : adapter_(std::move(adapter)),
      mods_(mod_root),
      state_root_(mod_root.parent_path() / ".enrage"),
      recovery_(state_root_) {
    plugins_.set_execution_observer(
        [this](std::string_view mod_id, std::string_view plugin_id, std::string_view callback) {
            const bool ok = recovery_.enter_execution(mod_id, plugin_id, callback);
            if (!ok) report_recovery_state_error("could not persist native execution recovery marker");
            return ok;
        },
        [this]() {
            const bool ok = recovery_.leave_execution();
            if (!ok) report_recovery_state_error("could not clear native execution recovery marker");
            return ok;
        });
    plugins_.set_virtual_file_reader([this](std::string_view logical_path) {
        return overlay_.read(std::filesystem::path{std::string{logical_path}});
    });
}

Runtime::~Runtime() {
    stop();
}

bool Runtime::start(bool force_safe_mode) {
    std::scoped_lock lock(runtime_mutex_);
    if (running_ || !adapter_ || !adapter_->probe()) return false;
    if (!recovery_.begin_session(force_safe_mode)) return false;

    if (!adapter_->initialize()) {
        recovery_.end_session();
        return false;
    }

    std::error_code cleanup_error;
    std::filesystem::remove_all(state_root_ / "reload", cleanup_error);
    active_reload_state_.clear();
    reload_generation_ = 0;

    loaded_mods_.clear();
    diagnostics_.clear();
    load_order_.clear();
    overlay_.clear();
    plugins_.unload_all();
    health_ = {};
    capabilities_ = adapter_->capabilities();
    plugins_.set_capabilities(capabilities_);
    package_set_valid_ = false;
    conflicts_reported_ = false;
    extensions_active_ = false;
    recovery_state_error_reported_ = false;

    mods_.discover();
    auto packages = PackageLoader::load(mods_, state_root_);
    loaded_mods_ = std::move(packages.mods);
    diagnostics_ = std::move(packages.diagnostics);
    sync_health(true);

    if (recovery_.previous_session_incomplete()) {
        if (recovery_.suspected_mod()) {
            const auto message = recovery_.suspected_execution()
                ? recovery_message(*recovery_.suspected_execution())
                : std::string{"previous session ended while this mod was activating; it has been quarantined"};
            diagnostics_.push_back({
                DiagnosticSeverity::warning,
                DiagnosticCode::mod_quarantined,
                {},
                *recovery_.suspected_mod(),
                message,
            });
        } else {
            diagnostics_.push_back({
                DiagnosticSeverity::warning,
                DiagnosticCode::recovery_safe_mode,
                {},
                {},
                "previous session ended unexpectedly; extensions are disabled for this launch",
            });
        }
    } else if (recovery_.safe_mode()) {
        diagnostics_.push_back({
            DiagnosticSeverity::info,
            DiagnosticCode::recovery_safe_mode,
            {},
            {},
            "safe mode is active; extensions are disabled for this launch",
        });
    }

    std::vector<ModSpec> specs;
    specs.reserve(loaded_mods_.size());
    for (const auto& mod : loaded_mods_) {
        if (recovery_.quarantined(mod.manifest.id)) {
            if (!recovery_.suspected_mod() || *recovery_.suspected_mod() != mod.manifest.id) {
                diagnostics_.push_back({
                    DiagnosticSeverity::warning,
                    DiagnosticCode::mod_quarantined,
                    mod.source,
                    mod.manifest.id,
                    "mod is quarantined and will remain disabled until cleared",
                });
            }
            continue;
        }
        specs.push_back(mod.manifest.to_spec());
    }

    auto resolution = ModGraph::resolve(specs);
    for (const auto& issue : resolution.issues) {
        diagnostics_.push_back({
            DiagnosticSeverity::error,
            DiagnosticCode::dependency_error,
            {},
            issue.source,
            issue_message(issue),
        });
    }

    package_set_valid_ = resolution.ok();
    load_order_ = std::move(resolution.order);
    running_ = true;

    if (!recovery_.safe_mode() && adapter_->environment_state() == adapters::EnvironmentState::local) {
        activate_extensions();
    }
    return true;
}

void Runtime::stop() noexcept {
    std::scoped_lock lock(runtime_mutex_);
    if (!running_) return;
    deactivate_extensions();
    adapter_->shutdown();
    recovery_.end_session();
    running_ = false;

    if (!active_reload_state_.empty()) {
        std::error_code error;
        std::filesystem::remove_all(active_reload_state_, error);
        active_reload_state_.clear();
    }
}

void Runtime::tick(double delta_seconds) {
    std::scoped_lock lock(runtime_mutex_);
    if (!running_) return;

    if (recovery_.safe_mode()) {
        if (extensions_active_) deactivate_extensions();
        return;
    }

    if (adapter_->environment_state() == adapters::EnvironmentState::local) {
        sync_capabilities();
        sync_health();
        if (!extensions_active_) activate_extensions();
        if (extensions_active_ && !plugins_.tick(delta_seconds)) deactivate_extensions();
        return;
    }

    if (extensions_active_) deactivate_extensions();
}

bool Runtime::reload_support() {
    std::scoped_lock lock(runtime_mutex_);
    if (!running_ || recovery_.safe_mode()) return false;

    const bool was_active = extensions_active_;
    if (was_active) deactivate_extensions();

    if (!adapter_->reload_support()) {
        if (was_active && adapter_->environment_state() == adapters::EnvironmentState::local) activate_extensions();
        return false;
    }

    sync_capabilities();
    sync_health();

    if (adapter_->environment_state() == adapters::EnvironmentState::local) activate_extensions();
    return true;
}

bool Runtime::reload_mods() {
    std::scoped_lock lock(runtime_mutex_);
    if (!running_ || recovery_.safe_mode()) return false;

    ModRegistry candidate_registry{mods_.root()};
    candidate_registry.discover();

    const auto staging = state_root_ / "reload" / std::to_string(++reload_generation_);
    std::error_code error;
    std::filesystem::remove_all(staging, error);
    error.clear();

    auto candidate_packages = PackageLoader::load(candidate_registry, staging);
    bool rejected = contains_error(candidate_packages.diagnostics);

    std::vector<ModSpec> specs;
    specs.reserve(candidate_packages.mods.size());
    for (const auto& mod : candidate_packages.mods) {
        if (!recovery_.quarantined(mod.manifest.id)) specs.push_back(mod.manifest.to_spec());
    }

    auto resolution = ModGraph::resolve(specs);
    if (!resolution.ok()) rejected = true;

    if (rejected) {
        diagnostics_.insert(
            diagnostics_.end(),
            candidate_packages.diagnostics.begin(),
            candidate_packages.diagnostics.end());
        for (const auto& issue : resolution.issues) {
            diagnostics_.push_back({
                DiagnosticSeverity::error,
                DiagnosticCode::dependency_error,
                {},
                issue.source,
                issue_message(issue),
            });
        }
        diagnostics_.push_back({
            DiagnosticSeverity::warning,
            DiagnosticCode::reload_rejected,
            mods_.root(),
            {},
            "mod reload rejected; live package set was left unchanged",
        });
        std::filesystem::remove_all(staging, error);
        return false;
    }

    const auto old_registry = mods_;
    const auto old_loaded_mods = loaded_mods_;
    const auto old_load_order = load_order_;
    const auto old_reload_state = active_reload_state_;
    const bool old_package_set_valid = package_set_valid_;
    const bool old_conflicts_reported = conflicts_reported_;
    const bool was_active = extensions_active_;

    if (was_active) deactivate_extensions();

    mods_ = std::move(candidate_registry);
    loaded_mods_ = std::move(candidate_packages.mods);
    load_order_ = std::move(resolution.order);
    package_set_valid_ = true;
    conflicts_reported_ = false;

    const auto diagnostic_start = diagnostics_.size();
    diagnostics_.insert(
        diagnostics_.end(),
        candidate_packages.diagnostics.begin(),
        candidate_packages.diagnostics.end());

    if (adapter_->environment_state() == adapters::EnvironmentState::local) activate_extensions();

    if (activation_failed_since(diagnostic_start)) {
        deactivate_extensions();
        mods_ = old_registry;
        loaded_mods_ = old_loaded_mods;
        load_order_ = old_load_order;
        package_set_valid_ = old_package_set_valid;
        conflicts_reported_ = old_conflicts_reported;
        active_reload_state_ = old_reload_state;

        diagnostics_.push_back({
            DiagnosticSeverity::warning,
            DiagnosticCode::reload_rejected,
            mods_.root(),
            {},
            "mod reload activation failed; previous package set was restored",
        });

        if (was_active && adapter_->environment_state() == adapters::EnvironmentState::local) {
            activate_extensions();
        }
        std::filesystem::remove_all(staging, error);
        return false;
    }

    active_reload_state_ = staging;
    if (!old_reload_state.empty() && old_reload_state != active_reload_state_) {
        std::filesystem::remove_all(old_reload_state, error);
    }
    return true;
}

bool Runtime::running() const noexcept { return running_; }
bool Runtime::extensions_active() const noexcept { return extensions_active_; }
bool Runtime::safe_mode() const noexcept { return recovery_.safe_mode(); }
std::uint64_t Runtime::capabilities() const noexcept { return capabilities_; }
const adapters::AdapterHealthSnapshot& Runtime::health() const noexcept { return health_; }
const ModRegistry& Runtime::mods() const noexcept { return mods_; }
const std::vector<LoadedMod>& Runtime::loaded_mods() const noexcept { return loaded_mods_; }
const std::vector<Diagnostic>& Runtime::diagnostics() const noexcept { return diagnostics_; }
const Overlay& Runtime::overlay() const noexcept { return overlay_; }
const PluginHost& Runtime::plugins() const noexcept { return plugins_; }
const std::vector<std::string>& Runtime::quarantined_mods() const noexcept { return recovery_.quarantined_mods(); }
const std::optional<RecoveryBreadcrumb>& Runtime::suspected_execution() const noexcept { return recovery_.suspected_execution(); }
const adapters::Adapter& Runtime::adapter() const noexcept { return *adapter_; }

bool Runtime::clear_quarantine(std::string_view mod_id) {
    std::scoped_lock lock(runtime_mutex_);
    return recovery_.clear_quarantine(mod_id);
}

bool Runtime::clear_all_quarantine() {
    std::scoped_lock lock(runtime_mutex_);
    return recovery_.clear_all_quarantine();
}

void Runtime::activate_extensions() {
    if (extensions_active_ || !package_set_valid_ || recovery_.safe_mode()) return;

    if ((capabilities_ & ENRAGE_CAPABILITY_VIRTUAL_FILES) != 0) mount_content();

    for (const auto& id : load_order_) {
        const auto found = std::ranges::find(loaded_mods_, id, [](const LoadedMod& mod) {
            return mod.manifest.id;
        });
        if (found == loaded_mods_.end()) continue;

        for (const auto& plugin : found->native_plugins) {
            if (!plugins_.load(plugin, id)) {
                diagnostics_.push_back({
                    DiagnosticSeverity::error,
                    DiagnosticCode::plugin_load_failed,
                    found->source,
                    id,
                    "failed to load native plugin: " + plugin.filename().string(),
                });
            }
        }
    }

    extensions_active_ = true;
}

void Runtime::deactivate_extensions() noexcept {
    plugins_.unload_all();
    overlay_.clear();
    extensions_active_ = false;
}

void Runtime::mount_content() {
    if ((capabilities_ & ENRAGE_CAPABILITY_VIRTUAL_FILES) == 0) return;

    for (std::size_t index = 0; index < load_order_.size(); ++index) {
        const auto& id = load_order_[index];
        const auto found = std::ranges::find(loaded_mods_, id, [](const LoadedMod& mod) {
            return mod.manifest.id;
        });
        if (found == loaded_mods_.end() || found->content_root.empty()) continue;

        const bool mounted = found->kind == PackageKind::archive
            ? overlay_.mount_archive(id, found->archive_path, found->content_root, static_cast<int>(index))
            : overlay_.mount({id, found->content_root, static_cast<int>(index)});
        if (!mounted) {
            diagnostics_.push_back({
                DiagnosticSeverity::error,
                DiagnosticCode::mount_failed,
                found->source,
                id,
                "failed to mount mod content",
            });
        }
    }

    if (!conflicts_reported_) {
        for (const auto& conflict : overlay_.conflicts()) {
            diagnostics_.push_back({
                DiagnosticSeverity::warning,
                DiagnosticCode::file_conflict,
                {},
                conflict.mounts.empty() ? std::string{} : conflict.mounts.front(),
                conflict_message(conflict),
            });
        }
        conflicts_reported_ = true;
    }
}

void Runtime::sync_capabilities() {
    const auto next = adapter_->capabilities();
    if (next == capabilities_) return;

    const auto previous = capabilities_;
    capabilities_ = next;
    plugins_.set_capabilities(capabilities_);

    if (!extensions_active_) return;

    const bool had_virtual_files = (previous & ENRAGE_CAPABILITY_VIRTUAL_FILES) != 0;
    const bool has_virtual_files = (capabilities_ & ENRAGE_CAPABILITY_VIRTUAL_FILES) != 0;
    if (had_virtual_files && !has_virtual_files) overlay_.clear();
    else if (!had_virtual_files && has_virtual_files) mount_content();
}

void Runtime::sync_health(bool initial) {
    auto next = adapter_->health_snapshot();

    for (const auto& anchor : next.anchors) {
        const auto* previous = find_anchor(health_, anchor.name);
        const bool changed = !previous || previous->state != anchor.state || previous->reason != anchor.reason;
        if (!changed) continue;
        if (initial && anchor.state == adapters::AnchorHealthState::resolved) continue;

        diagnostics_.push_back({
            anchor_severity(anchor.state),
            DiagnosticCode::anchor_health_changed,
            {},
            {},
            anchor_message(anchor),
        });
    }

    for (const auto& capability : next.capabilities) {
        const auto* previous = find_capability(health_, capability.capability);
        const bool changed = !previous ||
                             previous->advertised != capability.advertised ||
                             previous->available != capability.available ||
                             previous->blocking_anchors != capability.blocking_anchors ||
                             previous->degraded_anchors != capability.degraded_anchors;
        if (!changed) continue;

        const bool unhealthy = capability.advertised &&
                               (!capability.available || !capability.degraded_anchors.empty());
        if (initial && !unhealthy) continue;
        if (!capability.advertised && initial) continue;

        diagnostics_.push_back({
            unhealthy ? DiagnosticSeverity::warning : DiagnosticSeverity::info,
            DiagnosticCode::capability_health_changed,
            {},
            {},
            capability_message(capability),
        });
    }

    health_ = std::move(next);
}

void Runtime::report_recovery_state_error(std::string message) {
    if (recovery_state_error_reported_) return;
    recovery_state_error_reported_ = true;
    diagnostics_.push_back({
        DiagnosticSeverity::error,
        DiagnosticCode::recovery_state_error,
        {},
        {},
        std::move(message),
    });
}

bool Runtime::activation_failed_since(std::size_t diagnostic_index) const noexcept {
    if (diagnostic_index >= diagnostics_.size()) return false;
    return std::ranges::any_of(
        diagnostics_.begin() + static_cast<std::ptrdiff_t>(diagnostic_index),
        diagnostics_.end(),
        [](const Diagnostic& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::error &&
                   (diagnostic.code == DiagnosticCode::mount_failed ||
                    diagnostic.code == DiagnosticCode::plugin_load_failed);
        });
}

} // namespace enrage::core
