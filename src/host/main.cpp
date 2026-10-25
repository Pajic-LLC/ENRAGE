#include "adapters/null_adapter.hpp"
#include "adapters/profile_adapter.hpp"
#include "core/runtime.hpp"
#include "core/status.hpp"
#include "enrage/sdk.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>

namespace {

const char* severity_name(enrage::core::DiagnosticSeverity severity) {
    switch (severity) {
        case enrage::core::DiagnosticSeverity::info: return "info";
        case enrage::core::DiagnosticSeverity::warning: return "warning";
        case enrage::core::DiagnosticSeverity::error: return "error";
    }
    return "unknown";
}

const char* environment_name(enrage::adapters::EnvironmentState environment) {
    switch (environment) {
        case enrage::adapters::EnvironmentState::unknown: return "unknown";
        case enrage::adapters::EnvironmentState::local: return "local";
        case enrage::adapters::EnvironmentState::restricted: return "restricted";
    }
    return "unknown";
}

const char* anchor_state_name(enrage::adapters::AnchorHealthState state) {
    switch (state) {
        case enrage::adapters::AnchorHealthState::unresolved: return "UNRESOLVED";
        case enrage::adapters::AnchorHealthState::resolved: return "RESOLVED";
        case enrage::adapters::AnchorHealthState::degraded: return "DEGRADED";
        case enrage::adapters::AnchorHealthState::missing: return "MISSING";
    }
    return "UNRESOLVED";
}

const char* capability_name(std::uint64_t capability) {
    switch (capability) {
        case ENRAGE_CAPABILITY_TICK: return "TICK";
        case ENRAGE_CAPABILITY_VIRTUAL_FILES: return "VIRTUAL_FILES";
        case ENRAGE_CAPABILITY_NATIVE_CALLS: return "NATIVE_CALLS";
        case ENRAGE_CAPABILITY_ASSETS: return "ASSETS";
        default: return "UNKNOWN";
    }
}

std::optional<enrage::adapters::EnvironmentState> parse_environment(std::string_view value) {
    if (value == "unknown") return enrage::adapters::EnvironmentState::unknown;
    if (value == "local") return enrage::adapters::EnvironmentState::local;
    if (value == "restricted") return enrage::adapters::EnvironmentState::restricted;
    return std::nullopt;
}

void usage() {
    std::cerr
        << "usage: enrage_host [mod-root] [--safe] [--json]\n"
        << "       enrage_host [mod-root] --image <host.exe> --profiles <directory>\n"
        << "           [--environment unknown|local|restricted] [--safe] [--json]\n";
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path mod_root = "mods";
    std::optional<std::filesystem::path> image;
    std::optional<std::filesystem::path> profiles;
    auto environment = enrage::adapters::EnvironmentState::unknown;
    bool environment_set = false;
    bool mod_root_set = false;
    bool force_safe_mode = false;
    bool json = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--safe") {
            force_safe_mode = true;
            continue;
        }
        if (argument == "--json") {
            json = true;
            continue;
        }
        if (argument == "--image") {
            if (++i >= argc) {
                usage();
                return 2;
            }
            image = std::filesystem::path{argv[i]};
            continue;
        }
        if (argument == "--profiles") {
            if (++i >= argc) {
                usage();
                return 2;
            }
            profiles = std::filesystem::path{argv[i]};
            continue;
        }
        if (argument == "--environment") {
            if (++i >= argc) {
                usage();
                return 2;
            }
            const auto parsed = parse_environment(argv[i]);
            if (!parsed) {
                usage();
                return 2;
            }
            environment = *parsed;
            environment_set = true;
            continue;
        }
        if (argument.starts_with('-')) {
            std::cerr << "Unknown option: " << argument << '\n';
            usage();
            return 2;
        }
        if (mod_root_set) {
            std::cerr << "Only one mod root may be specified.\n";
            return 2;
        }
        mod_root = argument;
        mod_root_set = true;
    }

    if (image.has_value() != profiles.has_value()) {
        std::cerr << "--image and --profiles must be supplied together.\n";
        return 2;
    }
    if (!image && environment_set) {
        std::cerr << "--environment requires --image and --profiles.\n";
        return 2;
    }

    std::unique_ptr<enrage::adapters::Adapter> adapter;
    if (image) {
        adapter = std::make_unique<enrage::adapters::ProfileAdapter>(
            *image,
            *profiles,
            environment);
    } else {
        adapter = std::make_unique<enrage::adapters::NullAdapter>();
    }

    enrage::core::Runtime runtime{std::move(adapter), std::move(mod_root)};

    if (!runtime.start(force_safe_mode)) {
        std::cerr << "ENRAGE failed to initialize.\n";
        return 1;
    }

    if (json) {
        std::cout << enrage::core::runtime_status_json(runtime) << '\n';
        runtime.stop();
        return 0;
    }

    const auto build = runtime.adapter().build();
    std::cout << "ENRAGE runtime: " << runtime.adapter().name() << '\n';
    std::cout << "Mode: " << (runtime.safe_mode() ? "safe" : "normal") << '\n';
    std::cout << "Environment: " << environment_name(runtime.adapter().environment_state()) << '\n';
    std::cout << "Build: " << build.id << '\n';
    std::cout << "Fingerprint: "
              << (build.fingerprint ? build.fingerprint->hex() : std::string{"n/a"}) << '\n';
    std::cout << "Capabilities: " << runtime.capabilities() << '\n';

    if (!runtime.health().anchors.empty()) {
        std::cout << "Anchors:\n";
        for (const auto& anchor : runtime.health().anchors) {
            std::cout << "  " << anchor.name << " " << anchor_state_name(anchor.state);
            if (!anchor.reason.empty()) std::cout << " - " << anchor.reason;
            std::cout << '\n';
        }
    }

    if (!runtime.health().capabilities.empty()) {
        std::cout << "Capability health:\n";
        for (const auto& capability : runtime.health().capabilities) {
            std::cout << "  " << capability_name(capability.capability) << " "
                      << (capability.available ? "available" : "suppressed") << '\n';
        }
    }

    std::cout << "Discovered " << runtime.mods().candidates().size() << " mod candidate(s).\n";
    for (const auto& mod : runtime.mods().candidates()) {
        std::cout << "  " << mod.id << (mod.packaged ? " [package]" : " [directory]") << '\n';
    }

    if (!runtime.quarantined_mods().empty()) {
        std::cout << "Quarantined " << runtime.quarantined_mods().size() << " mod(s):\n";
        for (const auto& id : runtime.quarantined_mods()) {
            std::cout << "  " << id << '\n';
        }
    }

    for (const auto& diagnostic : runtime.diagnostics()) {
        std::cout << '[' << severity_name(diagnostic.severity) << "] ";
        if (!diagnostic.mod_id.empty()) std::cout << diagnostic.mod_id << ": ";
        std::cout << diagnostic.message << '\n';
    }

    runtime.stop();
    return 0;
}
