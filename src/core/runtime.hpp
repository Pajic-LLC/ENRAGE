#pragma once

#include "adapters/adapter.hpp"
#include "core/mod_registry.hpp"

#include <filesystem>
#include <memory>

namespace enrage::core {

class Runtime {
public:
    Runtime(std::unique_ptr<adapters::Adapter> adapter, std::filesystem::path mod_root);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool start();
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] const ModRegistry& mods() const noexcept;
    [[nodiscard]] const adapters::Adapter& adapter() const noexcept;

private:
    std::unique_ptr<adapters::Adapter> adapter_;
    ModRegistry mods_;
    bool running_{};
};

} // namespace enrage::core
