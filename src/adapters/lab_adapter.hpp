#pragma once

#include "adapters/adapter.hpp"
#include "adapters/capability_map.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace enrage::adapters {

struct LabState {
    std::string adapter_name{"ENRAGE Lab"};
    BuildIdentity build{"lab.1", 1};
    std::uint64_t capability_bits{};
    CapabilityMap capability_map;
    EnvironmentState environment{EnvironmentState::unknown};
    bool probe_result{true};
    bool initialize_result{true};
    bool initialized{};
    std::size_t probe_count{};
    std::size_t initialize_count{};
    std::size_t shutdown_count{};
    std::vector<std::string> events;
};

class LabAdapter final : public Adapter {
public:
    LabAdapter();
    explicit LabAdapter(std::shared_ptr<LabState> state);

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] bool probe() override;
    [[nodiscard]] BuildIdentity build() const override;
    [[nodiscard]] std::uint64_t capabilities() const noexcept override;
    [[nodiscard]] EnvironmentState environment_state() const noexcept override;
    [[nodiscard]] AdapterHealthSnapshot health_snapshot() const override;
    bool initialize() override;
    void shutdown() noexcept override;

    [[nodiscard]] const std::shared_ptr<LabState>& state() const noexcept;

private:
    std::shared_ptr<LabState> state_;
};

} // namespace enrage::adapters
