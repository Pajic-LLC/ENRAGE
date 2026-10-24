#include "adapters/null_adapter.hpp"
#include "core/runtime.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
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

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path mod_root = "mods";
    bool mod_root_set = false;
    bool force_safe_mode = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--safe") {
            force_safe_mode = true;
            continue;
        }
        if (argument.starts_with('-')) {
            std::cerr << "Unknown option: " << argument << '\n';
            return 2;
        }
        if (mod_root_set) {
            std::cerr << "Only one mod root may be specified.\n";
            return 2;
        }
        mod_root = argument;
        mod_root_set = true;
    }

    enrage::core::Runtime runtime{
        std::make_unique<enrage::adapters::NullAdapter>(),
        std::move(mod_root),
    };

    if (!runtime.start(force_safe_mode)) {
        std::cerr << "ENRAGE failed to initialize.\n";
        return 1;
    }

    std::cout << "ENRAGE runtime: " << runtime.adapter().name() << '\n';
    std::cout << "Mode: " << (runtime.safe_mode() ? "safe" : "normal") << '\n';
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
