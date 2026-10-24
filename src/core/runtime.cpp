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
    overlay_.clear();
    plugins_.unload_all();
    plugins_.set_capabilities(adapter_->capabilities());

    mods_.discover();
    auto packages = PackageLoader::load(mods_, mods_.root().parent_path() / ".enrage");
    loaded_mods_ = std::move(packages.mods);
    diagnostics_ = std::move(packages.diagnostics);

    std::vector<ModSpec> specs;
    specs.reserve(loaded_mods_.size());
    for (const auto& mod : loaded_mods_) {
        specs.push_back(mod.manifest.to_spec());
    }

    const auto resolution = ModGraph::resolve(specs);
    for (const auto& issue : resolution.issues) {
        diagnostics_.push_back({
            DiagnosticSeverity::error,
            DiagnosticCode::dependency_error,
            {},
            issue.source,
            issue_message(issue),
        });
    }

    if (resolution.ok()) {
        for (std::size_t index = 0; index < resolution.order.size(); ++index) {
            const auto& id = resolution.order[index];
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
                continue;
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
    }

    running_ = true;
    return true;
}

void Runtime::stop() noexcept {
    if (!running_) {
        return;
    }

    plugins_.unload_all();
    overlay_.clear();
    adapter_->shutdown();
    running_ = false;
}

void Runtime::tick(double delta_seconds) {
    if (running_) {
        plugins_.tick(delta_seconds);
    }
}

bool Runtime::running() const noexcept {
    return running_;
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

} // namespace enrage::core
