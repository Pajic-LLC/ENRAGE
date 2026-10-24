#pragma once

#include "adapters/adapter.hpp"
#include "core/mod_registry.hpp"
#include "core/overlay.hpp"
#include "core/package.hpp"
#include "core/plugin_host.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace enrage::core {

class Runtime {
public:
    Runtime(std::unique_ptr<adapters::Adapter> adapter, std::filesystem::path mod_root);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool start();
    void stop() noexcept;
    void tick(double delta_seconds);

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool extensions_active() const noexcept;
    [[nodiscard]] const ModRegistry& mods() const noexcept;
    [[nodiscard]] const std::vector<LoadedMod>& loaded_mods() const noexcept;
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept;
    [[nodiscard]] const Overlay& overlay() const noexcept;
    [[nodiscard]] const PluginHost& plugins() const noexcept;
    [[nodiscard]] const adapters::Adapter& adapter() const noexcept;

private:
    void activate_extensions();
    void deactivate_extensions() noexcept;

    std::unique_ptr<adapters::Adapter> adapter_;
    ModRegistry mods_;
    std::vector<LoadedMod> loaded_mods_;
    std::vector<Diagnostic> diagnostics_;
    std::vector<std::string> load_order_;
    Overlay overlay_;
    PluginHost plugins_;
    bool package_set_valid_{};
    bool conflicts_reported_{};
    bool extensions_active_{};
    bool running_{};
};

} // namespace enrage::core
