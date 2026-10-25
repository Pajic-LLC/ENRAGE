#include "core/status.hpp"

#include "core/runtime.hpp"
#include "enrage/sdk.h"

#include <cstdint>
#include <sstream>
#include <string_view>

namespace enrage::core {
namespace {

std::string escape_json(std::string_view input) {
    std::ostringstream out;
    for (const unsigned char ch : input) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    static constexpr char digits[] = "0123456789abcdef";
                    out << "\\u00" << digits[(ch >> 4) & 0xf] << digits[ch & 0xf];
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    return out.str();
}

std::string_view environment_name(adapters::EnvironmentState value) {
    switch (value) {
        case adapters::EnvironmentState::unknown: return "unknown";
        case adapters::EnvironmentState::local: return "local";
        case adapters::EnvironmentState::restricted: return "restricted";
    }
    return "unknown";
}

std::string_view anchor_state_name(adapters::AnchorHealthState value) {
    switch (value) {
        case adapters::AnchorHealthState::unresolved: return "unresolved";
        case adapters::AnchorHealthState::resolved: return "resolved";
        case adapters::AnchorHealthState::degraded: return "degraded";
        case adapters::AnchorHealthState::missing: return "missing";
    }
    return "unresolved";
}

std::string_view severity_name(DiagnosticSeverity value) {
    switch (value) {
        case DiagnosticSeverity::info: return "info";
        case DiagnosticSeverity::warning: return "warning";
        case DiagnosticSeverity::error: return "error";
    }
    return "error";
}

std::string_view diagnostic_code_name(DiagnosticCode value) {
    switch (value) {
        case DiagnosticCode::manifest_missing: return "manifest_missing";
        case DiagnosticCode::manifest_invalid: return "manifest_invalid";
        case DiagnosticCode::manifest_version_unsupported: return "manifest_version_unsupported";
        case DiagnosticCode::runtime_abi_mismatch: return "runtime_abi_mismatch";
        case DiagnosticCode::archive_invalid: return "archive_invalid";
        case DiagnosticCode::archive_extract_failed: return "archive_extract_failed";
        case DiagnosticCode::unsafe_path: return "unsafe_path";
        case DiagnosticCode::missing_content_root: return "missing_content_root";
        case DiagnosticCode::missing_plugin: return "missing_plugin";
        case DiagnosticCode::dependency_error: return "dependency_error";
        case DiagnosticCode::mount_failed: return "mount_failed";
        case DiagnosticCode::plugin_load_failed: return "plugin_load_failed";
        case DiagnosticCode::plugin_reload_failed: return "plugin_reload_failed";
        case DiagnosticCode::file_conflict: return "file_conflict";
        case DiagnosticCode::recovery_state_error: return "recovery_state_error";
        case DiagnosticCode::recovery_safe_mode: return "recovery_safe_mode";
        case DiagnosticCode::mod_quarantined: return "mod_quarantined";
        case DiagnosticCode::anchor_health_changed: return "anchor_health_changed";
        case DiagnosticCode::capability_health_changed: return "capability_health_changed";
        case DiagnosticCode::reload_rejected: return "reload_rejected";
    }
    return "unknown";
}

std::string_view package_kind_name(PackageKind value) {
    switch (value) {
        case PackageKind::directory: return "directory";
        case PackageKind::archive: return "archive";
    }
    return "directory";
}

void string_array(std::ostringstream& out, const std::vector<std::string>& values) {
    out << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) out << ',';
        out << '"' << escape_json(values[index]) << '"';
    }
    out << ']';
}

} // namespace

std::string runtime_status_json(const Runtime& runtime) {
    std::ostringstream out;
    const auto build = runtime.adapter().build();

    out << '{';
    out << "\"schema\":1";
    out << ",\"running\":" << (runtime.running() ? "true" : "false");
    out << ",\"safeMode\":" << (runtime.safe_mode() ? "true" : "false");
    out << ",\"extensionsActive\":" << (runtime.extensions_active() ? "true" : "false");
    out << ",\"adapter\":\"" << escape_json(runtime.adapter().name()) << '"';
    out << ",\"environment\":\"" << environment_name(runtime.adapter().environment_state()) << '"';
    out << ",\"build\":{";
    out << "\"id\":\"" << escape_json(build.id) << '"';
    if (build.fingerprint) {
        out << ",\"sha256\":\"" << build.fingerprint->hex() << '"';
        out << ",\"size\":" << build.fingerprint->size;
    } else {
        out << ",\"sha256\":null,\"size\":null";
    }
    out << '}';
    out << ",\"capabilities\":" << runtime.capabilities();

    out << ",\"anchors\":[";
    for (std::size_t index = 0; index < runtime.health().anchors.size(); ++index) {
        if (index != 0) out << ',';
        const auto& anchor = runtime.health().anchors[index];
        out << "{\"name\":\"" << escape_json(anchor.name)
            << "\",\"state\":\"" << anchor_state_name(anchor.state)
            << "\",\"reason\":\"" << escape_json(anchor.reason) << "\"}";
    }
    out << ']';

    out << ",\"capabilityHealth\":[";
    for (std::size_t index = 0; index < runtime.health().capabilities.size(); ++index) {
        if (index != 0) out << ',';
        const auto& capability = runtime.health().capabilities[index];
        out << "{\"capability\":" << capability.capability
            << ",\"advertised\":" << (capability.advertised ? "true" : "false")
            << ",\"available\":" << (capability.available ? "true" : "false")
            << ",\"requiredAnchors\":";
        string_array(out, capability.required_anchors);
        out << ",\"blockingAnchors\":";
        string_array(out, capability.blocking_anchors);
        out << ",\"degradedAnchors\":";
        string_array(out, capability.degraded_anchors);
        out << '}';
    }
    out << ']';

    out << ",\"mods\":[";
    for (std::size_t index = 0; index < runtime.loaded_mods().size(); ++index) {
        if (index != 0) out << ',';
        const auto& mod = runtime.loaded_mods()[index];
        out << "{\"id\":\"" << escape_json(mod.manifest.id)
            << "\",\"name\":\"" << escape_json(mod.manifest.name)
            << "\",\"version\":\"" << escape_json(mod.manifest.version)
            << "\",\"kind\":\"" << package_kind_name(mod.kind) << "\"}";
    }
    out << ']';

    out << ",\"plugins\":[";
    for (std::size_t index = 0; index < runtime.plugins().plugins().size(); ++index) {
        if (index != 0) out << ',';
        const auto& plugin = runtime.plugins().plugins()[index];
        out << "{\"id\":\"" << escape_json(plugin.id)
            << "\",\"name\":\"" << escape_json(plugin.name)
            << "\",\"version\":\"" << escape_json(plugin.version)
            << "\",\"ownerMod\":\"" << escape_json(plugin.owner_mod_id) << "\"}";
    }
    out << ']';

    out << ",\"quarantinedMods\":";
    string_array(out, runtime.quarantined_mods());

    out << ",\"diagnostics\":[";
    for (std::size_t index = 0; index < runtime.diagnostics().size(); ++index) {
        if (index != 0) out << ',';
        const auto& diagnostic = runtime.diagnostics()[index];
        out << "{\"severity\":\"" << severity_name(diagnostic.severity)
            << "\",\"code\":\"" << diagnostic_code_name(diagnostic.code)
            << "\",\"modId\":\"" << escape_json(diagnostic.mod_id)
            << "\",\"message\":\"" << escape_json(diagnostic.message) << "\"}";
    }
    out << ']';
    out << '}';
    return out.str();
}

} // namespace enrage::core
