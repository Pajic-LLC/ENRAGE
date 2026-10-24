#pragma once

#include "adapters/adapter.hpp"

namespace enrage::adapters {

class NullAdapter final : public Adapter {
public:
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] bool probe() override;
    [[nodiscard]] BuildIdentity build() const override;
    [[nodiscard]] std::uint64_t capabilities() const noexcept override;
    [[nodiscard]] EnvironmentState environment_state() const noexcept override;
    bool initialize() override;
    void shutdown() noexcept override;
};

} // namespace enrage::adapters
