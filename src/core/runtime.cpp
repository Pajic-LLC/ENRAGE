#include "core/runtime.hpp"

#include "core/mod_graph.hpp"

#include <algorithm>
#include <string>
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

} // namespace

Runtime::Runtime(std::unique_ptr<adapters::Adapter> adapter, std::filesystem::path mod_root)
    : adapter_(std::move(adapter)), mods_(std::move(mod_root)) {}

Runtime::~Runtime() {
    stop();
}

bool Runtime::start() {
    if (running_ || !adapter_ || !adapter_->probe()) {
        return false;
    }

    if (!adapter_->initialize()) {
        return false;
    }

    loaded_mods_.clear();
    diagnostics_.clear();
    load_order_.clear();
    overlay_.clear();
    plugins_.unload_all();
    plugins_.set_capabilities(adapter_->capabilities());
    package_set_valid_ = false;
    conflicts_reported_ = false;
    extensions_active_ = false;

    mods_.discover();
    auto packages = PackageLoader::load(mods_, mods_.root().parent_path() / ".enrage");
    loaded_mods_ = std::move(packages.mods);
    diagnostics_ = std::move(packages.diagnostics);

    std::vector<ModSpec> specs;
    specs.reserve(loaded_mods_.size());
    for (const auto& mod : loaded_mods_) {
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

    if (adapter_->environment_state() == adapters::EnvironmentState::local) {
        activate_extensions();
    }
    return true;
}

void Runtime::stop() noexcept {
    if (!running_) {
        return;
    }

    deactivate_extensions();
    adapter_->shutdown();
    running_ = false;
}

void Runtime::tick(double delta_seconds) {
    if (!running_) return;

    if (adapter_->environment_state() == adapters::EnvironmentState::local) {
        if (!extensions_active_) activate_extensions();
        if (extensions_active_) plugins_.tick(delta_seconds);
        return;
    }

    if (extensions_active_) deactivate_extensions();
}

bool Runtime::running() const noexcept {
    return running_;
}

bool Runtime::extensions_active() const noexcept {
    return extensions_active_;
}

const ModRegistry& Runtime::mods() const noexcept {
    return mods_;
}

const std::vector<LoadedMod>& Runtime::loaded_mods() const noexcept {
    return loaded_mods_;
}

const std::vector<Diagnostic>& Runtime::diagnostics() const noexcept {
    return diagnostics_;
}

const Overlay& Runtime::overlay() const noexcept {
    return overlay_;
}

const PluginHost& Runtime::plugins() const noexcept {
    return plugins_;
}

const adapters::Adapter& Runtime::adapter() const noexcept {
    return *adapter_;
}

void Runtime::activate_extensions() {
    if (extensions_active_ || !package_set_valid_) return;

    for (std::size_t index = 0; index < load_order_.size(); ++index) {
        const auto& id = load_order_[index];
        const auto found = std::ranges::find(loaded_mods_, id, [](const LoadedMod& mod) {
            return mod.manifest.id;
        });
        if (found == loaded_mods_.end()) continue;

        if (!found->content_root.empty() && !overlay_.mount({id, found->content_root, static_cast<int>(index)})) {
            diagnostics_.push_back({
                DiagnosticSeverity::error,
                DiagnosticCode::mount_failed,
                found->source,
                id,
                "failed to mount mod content",
            });
        }

        for (const auto& plugin : found->native_plugins) {
            if (!plugins_.load(plugin)) {
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

    extensions_active_ = true;
}

void Runtime::deactivate_extensions() noexcept {
    plugins_.unload_all();
    overlay_.clear();
    extensions_active_ = false;
}

} // namespace enrage::core
